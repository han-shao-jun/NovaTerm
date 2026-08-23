/**
 * @file   TerminalCore.cpp
 * @brief  终端核心 Qt facade 实现。
 *
 * 详见 TerminalCore.h 的接口说明。本文件实现：
 * - Runtime 内部类：持有 BoundedByteQueue、命令队列、模型互斥量、工作线程；
 *   以原子量记录字节/命令的提交与完成计数，实现 waitForIdle。
 * - 输入事件 → ParserCommand 转换并排队（通过 byteBarrier 与字节流保序）
 * - 模型查询：snapshot / rendererSnapshot 等（持 modelMutex）
 * - 信号合并发布：在两次模型锁释放间累积 damage、cursorChanged 等，
 *   一次性通过 QueuedConnection 投递到 GUI 线程
 */
#include "TerminalCore.h"

#include "KeyMapper.h"
#include "ScrollbackBuffer.h"
#include "VTAdapter.h"

#include <QDeadlineTimer>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QWaitCondition>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <deque>
#include <utility>
#include <vector>

namespace {

// 解析队列总容量：8 MiB，足够吸收一次大批量 paste/cat 输出。
constexpr qsizetype QueueCapacity = 8 * 1024 * 1024;
// 每次喂给 libvterm 的最大字节数；过大会延长单次模型锁持有时间。
constexpr qsizetype ParserBatchSize = 64 * 1024;
// 高水位：队列填充至此触发背压，建议上游停止投递。
constexpr qsizetype QueueHighWatermark = QueueCapacity * 3 / 4;
// 低水位：队列消费至此解除背压。
constexpr qsizetype QueueLowWatermark = QueueCapacity / 2;
// 命令队列最大条目数，防止 GUI 线程失控时无限堆积。
constexpr size_t MaximumPendingCommands = 4096;
// 命令队列预估占用上限，与 QueueCapacity 对齐。
constexpr qsizetype MaximumPendingCommandBytes = 8 * 1024 * 1024;

enum class CommandType
{
    KeyboardCharacter,
    KeyboardKey,
    MouseButton,
    Paste,
    Resize,
    DefaultColors,
    FocusIn,
    FocusOut,
    SetScrollbackLimit,
    ClearScrollback,
    Flush
};

// GUI 线程产生的待执行命令。所有输入事件（键盘、鼠标、resize、paste 等）
// 都先被打包成 ParserCommand 入队，再由 worker 线程串行消费，避免对
// libvterm 的并发访问。
struct ParserCommand
{
    CommandType type{CommandType::Flush};
    uint32_t codepoint{0};
    int first{0};
    int second{0};
    bool pressed{false};
    QString text;
    NovaTerm::TerminalColor foreground;
    NovaTerm::TerminalColor background;
    // 该命令入队时的 submittedBytes 快照。worker 线程据此判断：只有当
    // 已完成字节数 >= byteBarrier 时才能执行本命令，从而保证命令与
    // 字节流之间的相对顺序（命令"看到"它之前写入的所有解析结果）。
    uint64_t byteBarrier{0};
};

// 计算一行 Cell 内容的 64 位身份哈希（FNV-1a 64-bit 变体）。
// 渲染层用此哈希快速判断行内容是否变化，避免对未变行重做字形装配。
// 仅用作"是否相同"的判定，不保证无碰撞；冲突时最坏退化为一次多余的渲染。
quint64 rowContentIdentity(const NovaTerm::Cell* cells, int columns)
{
    quint64 hash = 1469598103934665603ull;
    const auto mix = [&hash](quint64 value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    for (int column = 0; cells && column < columns; ++column) {
        const NovaTerm::Cell& cell = cells[column];
        for (uint32_t scalar : cell.chars)
            mix(scalar);
        mix(cell.width);
        mix(quint8(cell.foreground.type));
        mix(cell.foreground.index);
        mix(cell.foreground.red | (cell.foreground.green << 8)
            | (cell.foreground.blue << 16));
        mix(quint8(cell.background.type));
        mix(cell.background.index);
        mix(cell.background.red | (cell.background.green << 8)
            | (cell.background.blue << 16));
        const auto& a = cell.attributes;
        quint64 attributes = quint64(a.bold)
            | (quint64(a.underline) << 1)
            | (quint64(a.italic) << 2)
            | (quint64(a.blink) << 3)
            | (quint64(a.reverse) << 4)
            | (quint64(a.strike) << 5)
            | (quint64(a.font) << 6)
            | (quint64(a.dwl) << 7)
            | (quint64(a.dhl) << 8)
            | (quint64(a.smallFont) << 9)
            | (quint64(a.baseline) << 10)
            | (quint64(a.protectedCell) << 11)
            | (quint64(a.dim) << 12)
            | (quint64(a.conceal) << 13)
            | (quint64(a.underlineStyle) << 14);
        mix(attributes);
    }
    return hash;
}

} // namespace

class TerminalCore::Runtime
{
public:
    Runtime(TerminalCore* owner, int columns, int rows)
        : owner(owner)
        , screen(columns, rows)
        , scrollback(1000)
        , bytes(QueueCapacity)
    {
        rowRevisions.fill(0, rows);
        thread = QThread::create([this]() { workerMain(); });
        thread->setObjectName(QStringLiteral("NovaTerm Parser Worker"));
        thread->start();
    }

    ~Runtime()
    {
        accepting.store(false, std::memory_order_release);
        waitForIdle(5000);
        stopping.store(true, std::memory_order_release);
        bytes.stop();
        if (thread) {
            thread->wait();
            delete thread;
        }
    }

    TerminalCore::InputWriteResult enqueueBytes(QByteArrayView data)
    {
        TerminalCore::InputWriteResult result;
        result.requestedBytes = data.size();
        qsizetype offset = 0;
        while (offset < data.size()
               && accepting.load(std::memory_order_acquire)) {
            const qsizetype length =
                std::min<qsizetype>(ParserBatchSize, data.size() - offset);
            qsizetype queuedBytes = 0;
            if (!bytes.enqueue(data.sliced(offset, length), 0,
                               &queuedBytes)) {
                setBackpressure(true);
                result.backpressured = true;
                break;
            }
            submittedBytes.fetch_add(uint64_t(length),
                                     std::memory_order_release);
            offset += length;
            result.acceptedBytes = offset;
            if (queuedBytes >= QueueHighWatermark)
                setBackpressure(true);
        }
        return result;
    }

    bool enqueueCommand(ParserCommand command)
    {
        QMutexLocker locker(&commandMutex);
        if (!accepting.load(std::memory_order_acquire))
            return false;
        // 记录入队时刻的已提交字节计数。worker 线程消费时据此等待：
        // 仅当 completedBytes >= byteBarrier 时才执行本命令，从而保证
        // 命令在它之前提交的字节流之后被处理。
        command.byteBarrier = submittedBytes.load(std::memory_order_acquire);
        const qsizetype commandBytes =
            qsizetype(sizeof(ParserCommand))
            + command.text.size() * qsizetype(sizeof(QChar));

        // 同类型状态命令在队尾合并：只保留最新值，避免连续 resize 或
        // flush 命令在队列中堆积。键盘/鼠标命令不合并（顺序敏感）。
        if ((command.type == CommandType::Resize
             || command.type == CommandType::DefaultColors
             || command.type == CommandType::SetScrollbackLimit
             || command.type == CommandType::Flush)
            && !commands.empty()
            && commands.back().type == command.type) {
            pendingCommandBytes -= estimatedCommandBytes(commands.back());
            commands.back() = std::move(command);
            pendingCommandBytes += commandBytes;
            return true;
        }

        if (commands.size() >= MaximumPendingCommands
            || commandBytes > MaximumPendingCommandBytes
            || pendingCommandBytes
                   > MaximumPendingCommandBytes - commandBytes) {
            reportOverload(QStringLiteral("parser command queue is full"));
            return false;
        }
        commands.push_back(std::move(command));
        pendingCommandBytes += commandBytes;
        submittedCommands.fetch_add(1, std::memory_order_release);
        return true;
    }

    static qsizetype estimatedCommandBytes(const ParserCommand& command)
    {
        return qsizetype(sizeof(ParserCommand))
            + command.text.size() * qsizetype(sizeof(QChar));
    }

    bool waitForIdle(int timeoutMs) const
    {
        const uint64_t targetBytes =
            submittedBytes.load(std::memory_order_acquire);
        const uint64_t targetCommands =
            submittedCommands.load(std::memory_order_acquire);
        QMutexLocker locker(&completionMutex);
        QDeadlineTimer deadline(timeoutMs);
        while (completedBytes.load(std::memory_order_acquire) < targetBytes
               || completedCommands.load(std::memory_order_acquire)
                      < targetCommands) {
            if (!completionChanged.wait(&completionMutex, deadline))
                return false;
        }
        return true;
    }

    // worker 线程主循环：libvterm 解析与命令执行均在此串行进行。
    // 每轮先消费已就绪命令（受 byteBarrier 约束），再取一批字节喂给
    // libvterm。模型锁（modelMutex）只在访问 ScreenBuffer/VTAdapter 时
    // 短暂持有，信号发布则脱离锁通过 QueuedConnection 投递到 GUI 线程。
    void workerMain()
    {
        createAdapter();

        while (!stopping.load(std::memory_order_acquire)) {
            const uint64_t processedCommands = processCommands();
            if (processedCommands > 0) {
                completedCommands.fetch_add(processedCommands,
                                            std::memory_order_release);
                notifyCompletion();
            }

            const QByteArray batch = bytes.take(ParserBatchSize, 5);
            if (!batch.isEmpty()) {
                if (bytes.statistics().queuedBytes <= QueueLowWatermark)
                    setBackpressure(false);
                {
                    QMutexLocker modelLocker(&modelMutex);
                    adapter->writeInput(batch);
                    adapter->flushDamage();
                    commitPendingModelRevision();
                }
                publishPendingSignals();
                completedBytes.fetch_add(uint64_t(batch.size()),
                                         std::memory_order_release);
                notifyCompletion();
            }
        }

        adapter.reset();
        setBackpressure(false);
        notifyCompletion();
    }

    void setBackpressure(bool paused)
    {
        if (backpressure.exchange(paused, std::memory_order_acq_rel) == paused)
            return;
        QMetaObject::invokeMethod(
            owner,
            [target = owner, paused]() {
                emit target->inputBackpressureChanged(paused);
            },
            Qt::QueuedConnection);
    }

    void reportOverload(const QString& reason)
    {
        QMetaObject::invokeMethod(
            owner,
            [target = owner, reason]() { emit target->inputOverload(reason); },
            Qt::QueuedConnection);
    }

    void createAdapter()
    {
        NovaTerm::VTAdapter::Observer observer;
        observer.output = [this](QByteArrayView data) {
            constexpr qsizetype OutputBatchSize = 64 * 1024;
            if (pendingOutput.isEmpty()
                || pendingOutput.back().size() + data.size() > OutputBatchSize) {
                pendingOutput.push_back(QByteArray(data.data(), data.size()));
            } else {
                pendingOutput.back().append(data.data(), data.size());
            }
        };
        observer.damage = [this](const NovaTerm::DirtyRegion& region) {
            // 保留稀疏的多个独立区域，待 RenderScheduler 合并。
            // 若直接合并成单一包围盒，相距较远的 Cell 改动会被放大为
            // 近乎全屏的更新。
            pendingDamage.push_back(region);
        };
        observer.cursorChanged = [this](const NovaTerm::CursorState& value) {
            cursor = value;
            cursorChanged = true;
        };
        observer.titleChanged = [this](const QString& value) {
            currentTitle = value;
            titleChanged = true;
        };
        observer.bell = [this]() {
            bellPending = true;
        };
        observer.scrollbackChanged = [this]() {
            scrollbackChanged = true;
        };
        observer.screenScrolled = [this](int rows) {
            pendingScreenScrollRows += rows;
        };

        QMutexLocker modelLocker(&modelMutex);
        adapter = std::make_unique<NovaTerm::VTAdapter>(
            screen.columns(), screen.rows(), screen, scrollback,
            std::move(observer));
    }

    // 从命令队列取出可执行命令并执行。可执行的判定条件是
    // byteBarrier <= completedBytes，即命令所等待的字节已全部解析完成。
    // 取出后先在 modelMutex 保护下逐条执行，再统一提交一次模型 revision
    // 并发布累积的信号，避免每条命令都触发一次跨线程投递。
    uint64_t processCommands()
    {
        std::deque<ParserCommand> local;
        {
            QMutexLocker locker(&commandMutex);
            const uint64_t bytesDone =
                completedBytes.load(std::memory_order_acquire);
            while (!commands.empty()
                   && commands.front().byteBarrier <= bytesDone) {
                pendingCommandBytes -=
                    estimatedCommandBytes(commands.front());
                local.push_back(std::move(commands.front()));
                commands.pop_front();
            }
        }
        if (local.empty())
            return 0;

        {
            QMutexLocker modelLocker(&modelMutex);
            for (const ParserCommand& command : local)
                executeCommand(command);
            commitPendingModelRevision();
        }
        publishPendingSignals();
        return uint64_t(local.size());
    }

    void executeCommand(const ParserCommand& command)
    {
        switch (command.type) {
        case CommandType::KeyboardCharacter:
            adapter->keyboardUnichar(command.codepoint, command.first);
            break;
        case CommandType::KeyboardKey:
            adapter->keyboardKey(command.first, command.second);
            break;
        case CommandType::MouseButton:
            adapter->mouseButton(command.first, command.pressed, command.second);
            break;
        case CommandType::Paste:
            adapter->startPaste();
            for (const uint32_t codepoint : command.text.toUcs4())
                adapter->keyboardUnichar(codepoint, VTERM_MOD_NONE);
            adapter->endPaste();
            break;
        case CommandType::Resize:
            adapter->resize(command.first, command.second);
            break;
        case CommandType::DefaultColors:
            adapter->setDefaultColors(command.foreground, command.background);
            break;
        case CommandType::FocusIn:
            adapter->focusIn();
            break;
        case CommandType::FocusOut:
            adapter->focusOut();
            break;
        case CommandType::SetScrollbackLimit:
            scrollback.setMaxLines(command.first);
            break;
        case CommandType::ClearScrollback:
            scrollback.clear();
            scrollbackChanged = true;
            break;
        case CommandType::Flush:
            adapter->flushDamage();
            break;
        }
    }

    // 将一次 modelMutex 释放区间内累积的所有信号一次性发布到 GUI 线程。
    // 通过 swap + std::exchange 把待发数据快速取出后即释放锁外投递，
    // 减少 QueuedConnection 的调用次数，避免高频 damage 导致 GUI 线程
    // 信号队列爆掉。
    void publishPendingSignals()
    {
        QVector<NovaTerm::DirtyRegion> damageValue;
        damageValue.swap(pendingDamage);
        const bool cursorValue = std::exchange(cursorChanged, false);
        const bool titleValue = std::exchange(titleChanged, false);
        const bool bellValue = std::exchange(bellPending, false);
        const bool scrollbackValue = std::exchange(scrollbackChanged, false);
        const int screenScrollRows = std::exchange(pendingScreenScrollRows, 0);
        QVector<QByteArray> output;
        output.swap(pendingOutput);
        const quint64 revisionValue = std::exchange(pendingRevision, 0);

        if (damageValue.isEmpty() && !cursorValue && !titleValue && !bellValue
            && !scrollbackValue && screenScrollRows == 0 && output.isEmpty()) {
            return;
        }

        QString titleCopy;
        if (titleValue) {
            QMutexLocker locker(&modelMutex);
            titleCopy = currentTitle;
        }

        QMetaObject::invokeMethod(
            owner,
            [target = owner, damageValue, revisionValue, cursorValue,
             titleValue, titleCopy, bellValue, scrollbackValue, screenScrollRows,
             output = std::move(output)]() {
                for (const QByteArray& data : output)
                    emit target->outputData(data);
                if (screenScrollRows > 0)
                    emit target->screenScrolled(screenScrollRows);
                for (const NovaTerm::DirtyRegion& region : damageValue)
                    emit target->damage(region, revisionValue);
                if (cursorValue)
                    emit target->cursorMoved();
                if (titleValue)
                    emit target->titleChanged(titleCopy);
                if (bellValue)
                    emit target->bell();
                if (scrollbackValue)
                    emit target->scrollbackChanged();
            },
            Qt::QueuedConnection);
    }

    void commitPendingModelRevision()
    {
        // 调用方须持有 modelMutex，且在一次 adapter/command 批次结束后调用。
        // 因此一次 revision 描述一次稳定的模型发布，并对应于该发布期间
        // 发出的所有 damage 区域。
        if (pendingDamage.isEmpty() && !cursorChanged && !scrollbackChanged)
            return;
        pendingRevision = ++modelRevision;
        if (rowRevisions.size() != screen.rows()) {
            rowRevisions.fill(modelRevision, screen.rows());
            return;
        }
        for (const NovaTerm::DirtyRegion& region :
             std::as_const(pendingDamage)) {
            const int start = std::clamp(region.startRow, 0, screen.rows());
            const int end = std::clamp(region.endRow, 0, screen.rows());
            for (int row = start; row < end; ++row)
                rowRevisions[row] = modelRevision;
        }
    }

    void notifyCompletion()
    {
        QMutexLocker locker(&completionMutex);
        completionChanged.wakeAll();
    }

    TerminalCore* owner;
    mutable QMutex modelMutex;
    NovaTerm::ScreenBuffer screen;
    ScrollbackBuffer scrollback;
    NovaTerm::CursorState cursor;
    QString currentTitle;
    quint64 modelRevision{0};
    quint64 pendingRevision{0};
    QVector<quint64> rowRevisions;

    NovaTerm::BoundedByteQueue bytes;
    mutable QMutex commandMutex;
    std::deque<ParserCommand> commands;
    qsizetype pendingCommandBytes{0};

    mutable QMutex completionMutex;
    mutable QWaitCondition completionChanged;
    std::atomic<uint64_t> submittedBytes{0};
    std::atomic<uint64_t> completedBytes{0};
    std::atomic<uint64_t> submittedCommands{0};
    std::atomic<uint64_t> completedCommands{0};
    std::atomic<bool> accepting{true};
    std::atomic<bool> stopping{false};
    std::atomic<bool> backpressure{false};
    QThread* thread{nullptr};
    std::unique_ptr<NovaTerm::VTAdapter> adapter;

    QVector<NovaTerm::DirtyRegion> pendingDamage;
    QVector<QByteArray> pendingOutput;
    bool cursorChanged{false};
    bool titleChanged{false};
    bool bellPending{false};
    bool scrollbackChanged{false};
    int pendingScreenScrollRows{0};
};

TerminalCore::TerminalCore(int cols, int rows, QObject* parent)
    : QObject(parent)
    , _runtime(std::make_unique<Runtime>(this, cols, rows))
    , _searchEngine(std::make_unique<NovaTerm::SearchEngine>())
    , _reflowEngine(std::make_unique<NovaTerm::ReflowEngine>())
{
    connect(_searchEngine.get(), &NovaTerm::SearchEngine::resultsReady,
            this, &TerminalCore::searchResultsReady);
    connect(_reflowEngine.get(), &NovaTerm::ReflowEngine::batchReady,
            this, &TerminalCore::reflowBatchReady);
}

TerminalCore::~TerminalCore() = default;

TerminalCore::InputWriteResult TerminalCore::writeInput(QByteArrayView data)
{
    return _runtime->enqueueBytes(data);
}

void TerminalCore::processKeyPress(QKeyEvent* event)
{
    if (!event)
        return;

    const QString text = event->text();
    const int qtKey = event->key();
    const auto qtModifiers = event->modifiers();
    const int modifiers = int(KeyMapper::qtModToVTermMod(qtModifiers));

    if (qtModifiers.testFlag(Qt::ControlModifier)) {
        uint32_t controlCodepoint = 0;
        if (KeyMapper::qtKeyToControlCharacter(qtKey, controlCodepoint)) {
            ParserCommand command;
            command.type = CommandType::KeyboardCharacter;
            command.codepoint = controlCodepoint;
            // codepoint 已施加 Ctrl。保留 Alt/Shift 让 libvterm 应用其语义，
            // 但不再二次施加 Ctrl。
            command.first = modifiers & ~int(VTERM_MOD_CTRL);
            _runtime->enqueueCommand(std::move(command));
            return;
        }
    }

    if (!text.isEmpty() && text[0].isPrint()) {
        processTextInput(text, qtModifiers);
        return;
    }

    VTermKey key;
    if (KeyMapper::qtKeyToVTermKey(qtKey, key)) {
        ParserCommand command;
        command.type = CommandType::KeyboardKey;
        command.first = int(key);
        command.second = modifiers;
        _runtime->enqueueCommand(std::move(command));
        return;
    }

    processTextInput(text, qtModifiers);
}

void TerminalCore::processTextInput(const QString& text,
                                    Qt::KeyboardModifiers modifiers)
{
    const int vtermModifiers = int(KeyMapper::qtModToVTermMod(modifiers));
    for (const uint32_t codepoint : text.toUcs4()) {
        ParserCommand command;
        command.type = CommandType::KeyboardCharacter;
        command.codepoint = codepoint;
        command.first = vtermModifiers;
        _runtime->enqueueCommand(std::move(command));
    }
}

void TerminalCore::processMousePress(QMouseEvent* event)
{
    if (!event)
        return;
    ParserCommand command;
    command.type = CommandType::MouseButton;
    command.first = event->button() == Qt::RightButton ? 2
        : event->button() == Qt::MiddleButton ? 3 : 1;
    command.second =
        int(KeyMapper::qtModToVTermMod(event->modifiers()));
    command.pressed = true;
    _runtime->enqueueCommand(std::move(command));
}

void TerminalCore::processMouseMove(QMouseEvent* event)
{
    Q_UNUSED(event);
}

void TerminalCore::processMouseRelease(QMouseEvent* event)
{
    if (!event)
        return;
    ParserCommand command;
    command.type = CommandType::MouseButton;
    command.first = event->button() == Qt::RightButton ? 2
        : event->button() == Qt::MiddleButton ? 3 : 1;
    command.second =
        int(KeyMapper::qtModToVTermMod(event->modifiers()));
    command.pressed = false;
    _runtime->enqueueCommand(std::move(command));
}

void TerminalCore::processWheel(QWheelEvent* event)
{
    if (!event || event->angleDelta().y() == 0)
        return;
    const int button = event->angleDelta().y() > 0 ? 4 : 5;
    const int modifiers =
        int(KeyMapper::qtModToVTermMod(event->modifiers()));
    // 鼠标滚轮在终端协议中等价于一次"按下+释放"的鼠标按键（按键 4=上滚，
    // 按键 5=下滚），因此对一次 wheel 事件成对投递两条命令。
    for (const bool pressed : {true, false}) {
        ParserCommand command;
        command.type = CommandType::MouseButton;
        command.first = button;
        command.second = modifiers;
        command.pressed = pressed;
        _runtime->enqueueCommand(std::move(command));
    }
}

void TerminalCore::focusIn()
{
    ParserCommand command;
    command.type = CommandType::FocusIn;
    _runtime->enqueueCommand(std::move(command));
}

void TerminalCore::focusOut()
{
    ParserCommand command;
    command.type = CommandType::FocusOut;
    _runtime->enqueueCommand(std::move(command));
}

void TerminalCore::pasteText(const QString& text)
{
    if (text.isEmpty())
        return;
    // 终端粘贴语义：把 Windows/网页常见的 CRLF 以及孤立 LF 规范化为 CR。
    // raw 模式 PTY 会把 \r 和 \n 各当作一次回车，若原样发送 \r\n，
    // readline/tty 会执行两次换行，导致粘贴的每一行重复或出现空行。
    // 统一用 \r（与 Enter 键一致，xterm/Windows Terminal 同款约定）：
    // Linux canonical tty 经 ICRNL 转 \n、readline raw 模式视为确认；
    // Windows ConPTY 也把 \r 视为 Enter，跨平台行为一致。
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\r"));
    normalized.replace(QChar(0x0a), QChar(0x0d));
    if (normalized.isEmpty())
        return;
    ParserCommand command;
    command.type = CommandType::Paste;
    command.text = std::move(normalized);
    _runtime->enqueueCommand(std::move(command));
}

void TerminalCore::resize(int cols, int rows)
{
    if (cols < 2 || rows < 1)
        return;
    ParserCommand command;
    command.type = CommandType::Resize;
    command.first = cols;
    command.second = rows;
    _runtime->enqueueCommand(std::move(command));
}

int TerminalCore::columns() const
{
    QMutexLocker locker(&_runtime->modelMutex);
    return _runtime->screen.columns();
}

int TerminalCore::rows() const
{
    QMutexLocker locker(&_runtime->modelMutex);
    return _runtime->screen.rows();
}

bool TerminalCore::getCell(int row, int col, NovaTerm::Cell& out) const
{
    QMutexLocker locker(&_runtime->modelMutex);
    const NovaTerm::Cell* cell = _runtime->screen.cellAt(row, col);
    if (!cell)
        return false;
    out = *cell;
    return true;
}

NovaTerm::TerminalSnapshot TerminalCore::snapshot() const
{
    QMutexLocker locker(&_runtime->modelMutex);
    NovaTerm::TerminalSnapshot result =
        NovaTerm::makeSnapshot(_runtime->screen, _runtime->cursor);
    result.revision = _runtime->modelRevision;
    return result;
}

NovaTerm::RendererSnapshot TerminalCore::rendererSnapshot(
    const QVector<bool>& dirtyRows, int scrollLine,
    NovaTerm::LineId anchorLine, qsizetype anchorWrap) const
{
    QMutexLocker locker(&_runtime->modelMutex);
    NovaTerm::RendererSnapshot snapshot;
    snapshot.revision = _runtime->modelRevision;
    snapshot.columns = _runtime->screen.columns();
    snapshot.rows = _runtime->screen.rows();
    snapshot.cursor = _runtime->cursor;
    snapshot.visibleRowRevisions.resize(snapshot.rows);
    snapshot.visibleRowIdentities.resize(snapshot.rows);
    snapshot.visibleRows.resize(snapshot.rows);

    // dirtyRows 与当前行数不一致时（窗口刚 resize 过），无法按位判断
    // 脏行，只能强制全量拷贝所有行。
    const bool copyAllRows = dirtyRows.size() != snapshot.rows;
    NovaTerm::ScrollbackSnapshot history;
    NovaTerm::ViewportSnapshot historyViewport;
    if (scrollLine > 0) {
        history = _runtime->scrollback.snapshot();
    }
    // 滚动回看时按 anchorLine+anchorWrap 或末尾行 + scrollLine 计算视口。
    // LineLayout::viewport 负责把逻辑行按当前列宽重新折行，输出
    // 每个可见 widget 行对应的逻辑行 ID 及 Cell 切片范围。
    if (!history.empty()) {
        const NovaTerm::LogicalLine* anchor = anchorLine != 0
            ? history.lineById(anchorLine)
            : history.lineAt(std::max<qsizetype>(
                  0, history.lineCount() - scrollLine));
        if (anchor) {
            historyViewport = NovaTerm::LineLayout::viewport(
                history, anchor->id, anchorWrap, snapshot.columns,
                std::min<qsizetype>(scrollLine, snapshot.rows), 0,
                history.version());
        }
    }
    // widgetRow 是渲染器视角下的行号；screenRow 是它在活动屏幕中的位置。
    // screenRow < 0 表示该行落在 scrollback 中，需要从 historyViewport 取。
    for (int widgetRow = 0; widgetRow < snapshot.rows; ++widgetRow) {
        const int screenRow = widgetRow - scrollLine;
        snapshot.visibleRowRevisions[widgetRow] = screenRow >= 0
            && screenRow < _runtime->rowRevisions.size()
            ? _runtime->rowRevisions[screenRow]
            : _runtime->modelRevision;
        // 渲染器声明该行未脏：只回填身份哈希，跳过 Cell 拷贝。
        if (!copyAllRows && !dirtyRows[widgetRow]) {
            if (screenRow >= 0)
                snapshot.visibleRowIdentities[widgetRow] =
                    rowContentIdentity(
                        _runtime->screen.cellAt(screenRow, 0),
                        snapshot.columns);
            continue;
        }
        QVector<NovaTerm::Cell> destination;
        destination.resize(snapshot.columns);
        // 该行位于 scrollback 视口：从对应的逻辑行切片拷贝。
        if (screenRow < 0) {
            if (widgetRow < historyViewport.rows.size()) {
                const auto& display = historyViewport.rows[widgetRow];
                const auto* logical = history.lineById(display.lineId);
                if (logical) {
                    const qsizetype count = std::min<qsizetype>(
                        snapshot.columns, display.endCell - display.startCell);
                    std::copy_n(logical->cells.cbegin() + display.startCell,
                                count, destination.begin());
                }
            }
            snapshot.visibleRows[widgetRow] =
                QSharedPointer<const QVector<NovaTerm::Cell>>::create(
                    std::move(destination));
            continue;
        }
        // 该行位于活动屏幕：直接整行拷贝。
        const NovaTerm::Cell* source = _runtime->screen.cellAt(screenRow, 0);
        if (source)
            std::copy_n(source, snapshot.columns, destination.begin());
        snapshot.visibleRowIdentities[widgetRow] =
            rowContentIdentity(destination.constData(), destination.size());
        snapshot.visibleRows[widgetRow] =
            QSharedPointer<const QVector<NovaTerm::Cell>>::create(
                std::move(destination));
    }
    return snapshot;
}

quint64 TerminalCore::modelRevision() const
{
    QMutexLocker locker(&_runtime->modelMutex);
    return _runtime->modelRevision;
}

NovaTerm::CursorState TerminalCore::cursorState() const
{
    QMutexLocker locker(&_runtime->modelMutex);
    return _runtime->cursor;
}

void TerminalCore::flushDamage()
{
    ParserCommand command;
    command.type = CommandType::Flush;
    _runtime->enqueueCommand(std::move(command));
}

void TerminalCore::setDefaultColors(
    const NovaTerm::TerminalColor& foreground,
    const NovaTerm::TerminalColor& background)
{
    ParserCommand command;
    command.type = CommandType::DefaultColors;
    command.foreground = foreground;
    command.background = background;
    _runtime->enqueueCommand(std::move(command));
}

int TerminalCore::scrollbackLineCount() const
{
    QMutexLocker locker(&_runtime->modelMutex);
    return _runtime->scrollback.lineCount();
}

bool TerminalCore::getScrollbackCell(int lineIndex, int col,
                                     NovaTerm::Cell& out) const
{
    QMutexLocker locker(&_runtime->modelMutex);
    const auto* line = _runtime->scrollback.lineVectorAt(lineIndex);
    if (!line || col < 0 || col >= _runtime->scrollback.columns())
        return false;
    out = col < line->size() ? line->at(col) : NovaTerm::Cell{};
    return true;
}

void TerminalCore::setScrollbackLimit(int lines)
{
    ParserCommand command;
    command.type = CommandType::SetScrollbackLimit;
    command.first = lines;
    _runtime->enqueueCommand(std::move(command));
}

void TerminalCore::clearScrollback()
{
    ParserCommand command;
    command.type = CommandType::ClearScrollback;
    _runtime->enqueueCommand(std::move(command));
}

NovaTerm::ScrollbackSnapshot TerminalCore::scrollbackSnapshot() const
{
    QMutexLocker locker(&_runtime->modelMutex);
    return _runtime->scrollback.snapshot();
}

NovaTerm::ScrollbackStatistics TerminalCore::scrollbackStatistics() const
{
    QMutexLocker locker(&_runtime->modelMutex);
    return _runtime->scrollback.statistics();
}

void TerminalCore::searchScrollback(NovaTerm::SearchRequest request)
{
    _searchEngine->search(scrollbackSnapshot(), std::move(request));
}

void TerminalCore::cancelSearch(quint64 generation)
{
    _searchEngine->cancel(generation);
}

void TerminalCore::requestScrollbackReflow(int columns, quint64 generation,
                                           qsizetype batchLines)
{
    _reflowEngine->request(scrollbackSnapshot(), columns, generation,
                           batchLines);
}

void TerminalCore::cancelScrollbackReflow(quint64 generation)
{
    _reflowEngine->cancel(generation);
}

NovaTerm::Position TerminalCore::cursorPosition() const
{
    QMutexLocker locker(&_runtime->modelMutex);
    return _runtime->cursor.position;
}

bool TerminalCore::cursorVisible() const
{
    QMutexLocker locker(&_runtime->modelMutex);
    return _runtime->cursor.visible;
}

NovaTerm::CursorShape TerminalCore::cursorShape() const
{
    QMutexLocker locker(&_runtime->modelMutex);
    return _runtime->cursor.shape;
}

bool TerminalCore::cursorBlink() const
{
    QMutexLocker locker(&_runtime->modelMutex);
    return _runtime->cursor.blink;
}

QString TerminalCore::title() const
{
    QMutexLocker locker(&_runtime->modelMutex);
    return _runtime->currentTitle;
}

bool TerminalCore::waitForIdle(int timeoutMs) const
{
    return _runtime->waitForIdle(timeoutMs);
}

NovaTerm::BoundedByteQueue::Statistics TerminalCore::queueStatistics() const
{
    return _runtime->bytes.statistics();
}
