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
#include "core/ThreadNaming.h"

#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// TerminalCore/Runtime 处于全局命名空间，引入核心整数别名。
using NovaTerm::isize;
using NovaTerm::u64;
using NovaTerm::u32;
using NovaTerm::u8;

namespace {

// 解析队列总容量：8 MiB，足够吸收一次大批量 paste/cat 输出。
constexpr isize QueueCapacity = 8 * 1024 * 1024;
// 每次喂给 libvterm 的最大字节数；过大会延长单次模型锁持有时间。
constexpr isize ParserBatchSize = 64 * 1024;
// 高水位：队列填充至此触发背压，建议上游停止投递。
constexpr isize QueueHighWatermark = QueueCapacity * 3 / 4;
// 低水位：队列消费至此解除背压。
constexpr isize QueueLowWatermark = QueueCapacity / 2;
// 命令队列最大条目数，防止 GUI 线程失控时无限堆积。
constexpr size_t MaximumPendingCommands = 4096;
// 命令队列预估占用上限，与 QueueCapacity 对齐。
constexpr isize MaximumPendingCommandBytes = 8 * 1024 * 1024;

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
    Flush,
    PublishContext,
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

// 计算一整行（columns 宽）Cell 内容的 64 位身份哈希（FNV-1a 64-bit 变体）。
// cells[0..count) 是真实 Cell，[count, columns) 视为默认空 Cell —— 历史行
// 切片可能短于 columns，按渲染时的补白布局哈希，使脏/非脏两条路径对同一行
// 得到一致 identity。渲染层用此哈希快速判断行内容是否变化，避免对未变行重做
// 字形装配。仅用作"是否相同"的判定，不保证无碰撞；冲突时最坏退化为一次多余
// 的渲染。
u64 rowContentIdentity(const NovaTerm::Cell* cells, int count, int columns,
                       std::vector<u64>* blockIdentities = nullptr)
{
    static const NovaTerm::Cell kDefaultCell{};
    constexpr u64 OffsetBasis = 1469598103934665603ull;
    const auto mix = [](u64& hash, u64 value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    const auto mixCell = [&mix](u64& hash, const NovaTerm::Cell& cell) {
        for (const uint32_t scalar : cell.chars)
            mix(hash, scalar);
        mix(hash, cell.width);
        mix(hash, u8(cell.foreground.type));
        mix(hash, cell.foreground.index);
        mix(hash, cell.foreground.red | (cell.foreground.green << 8)
            | (cell.foreground.blue << 16));
        mix(hash, u8(cell.background.type));
        mix(hash, cell.background.index);
        mix(hash, cell.background.red | (cell.background.green << 8)
            | (cell.background.blue << 16));
        const auto& attributes = cell.attributes;
        const u64 flags = u64(attributes.bold)
            | (u64(attributes.underline) << 1)
            | (u64(attributes.italic) << 2)
            | (u64(attributes.blink) << 3)
            | (u64(attributes.reverse) << 4)
            | (u64(attributes.strike) << 5)
            | (u64(attributes.font) << 6)
            | (u64(attributes.dwl) << 7)
            | (u64(attributes.dhl) << 8)
            | (u64(attributes.smallFont) << 9)
            | (u64(attributes.baseline) << 10)
            | (u64(attributes.protectedCell) << 11)
            | (u64(attributes.dim) << 12)
            | (u64(attributes.conceal) << 13)
            | (u64(attributes.underlineStyle) << 14);
        mix(hash, flags);
    };

    columns = std::max(0, columns);
    count = std::clamp(count, 0, columns);
    constexpr int BlockColumns =
        NovaTerm::RendererSnapshot::IdentityBlockColumns;
    const int blockCount = (columns + BlockColumns - 1) / BlockColumns;
    if (blockIdentities)
        blockIdentities->resize(std::size_t(blockCount));

    u64 rowHash = OffsetBasis;
    for (int block = 0; block < blockCount; ++block) {
        const int start = block * BlockColumns;
        const int end = std::min(columns, start + BlockColumns);
        u64 blockHash = OffsetBasis;
        for (int column = start; column < end; ++column) {
            const NovaTerm::Cell& cell =
                (cells && column < count) ? cells[column] : kDefaultCell;
            mixCell(blockHash, cell);
        }
        if (blockIdentities)
            (*blockIdentities)[std::size_t(block)] = blockHash;
        mix(rowHash, blockHash);
        mix(rowHash, u64(end - start));
    }
    return rowHash;
}

// ── Qt 事件 → 核心输入类型的翻译（门面层职责）──
// 核心层 KeyMapper 只认 NovaTerm::Key / KeyModifier，不依赖 Qt。这里把
// Qt::Key 与 Qt::KeyboardModifiers 翻译过去；阶段 7 建门面后这两个函数
// 连同 processKeyPress 等一并移入 coreqt 的 QtKeyTranslator。
NovaTerm::KeyModifier coreModsFromQt(Qt::KeyboardModifiers mods)
{
    NovaTerm::KeyModifier result = NovaTerm::KeyModifier::None;
    if (mods.testFlag(Qt::ShiftModifier))
        result |= NovaTerm::KeyModifier::Shift;
    if (mods.testFlag(Qt::AltModifier))
        result |= NovaTerm::KeyModifier::Alt;
    if (mods.testFlag(Qt::ControlModifier))
        result |= NovaTerm::KeyModifier::Ctrl;
    return result;
}

NovaTerm::Key coreKeyFromQt(int qtKey)
{
    using NovaTerm::Key;
    switch (qtKey) {
    case Qt::Key_Enter:
    case Qt::Key_Return:    return Key::Enter;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:   return Key::Tab;
    case Qt::Key_Backspace: return Key::Backspace;
    case Qt::Key_Escape:    return Key::Escape;
    case Qt::Key_Up:        return Key::Up;
    case Qt::Key_Down:      return Key::Down;
    case Qt::Key_Left:      return Key::Left;
    case Qt::Key_Right:     return Key::Right;
    case Qt::Key_Insert:    return Key::Insert;
    case Qt::Key_Delete:    return Key::Delete;
    case Qt::Key_Home:      return Key::Home;
    case Qt::Key_End:       return Key::End;
    case Qt::Key_PageUp:    return Key::PageUp;
    case Qt::Key_PageDown:  return Key::PageDown;
    case Qt::Key_F1:  return Key::F1;
    case Qt::Key_F2:  return Key::F2;
    case Qt::Key_F3:  return Key::F3;
    case Qt::Key_F4:  return Key::F4;
    case Qt::Key_F5:  return Key::F5;
    case Qt::Key_F6:  return Key::F6;
    case Qt::Key_F7:  return Key::F7;
    case Qt::Key_F8:  return Key::F8;
    case Qt::Key_F9:  return Key::F9;
    case Qt::Key_F10: return Key::F10;
    case Qt::Key_F11: return Key::F11;
    case Qt::Key_F12: return Key::F12;
    default:          return Key::None;
    }
}

// 鼠标按键 → 终端协议按键号：左键 1、右键 2、中键 3（其余按左键处理）。
int vtermMouseButton(Qt::MouseButton button)
{
    if (button == Qt::RightButton)
        return 2;
    if (button == Qt::MiddleButton)
        return 3;
    return 1;
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
        rowRevisions.assign(std::size_t(rows), 0);
        thread = std::thread([this]() { workerMain(); });
    }

    ~Runtime()
    {
        // 关闭时不排空解析队列：积压的字节无人再消费，排空只会让 GUI 线程在
        // 大量输出中关标签页时阻塞（旧实现 waitForIdle(5000) 最多等 5 秒）。
        // 停止接收、唤醒并置停即可；worker 完成当前这一批（至多一批 64 KiB）
        // 后就会看到 stopping 退出，join 因而是有界的。
        accepting.store(false, std::memory_order_release);
        stopping.store(true, std::memory_order_release);
        bytes.stop();
        if (thread.joinable())
            thread.join();
    }

    TerminalCore::InputWriteResult enqueueBytes(NovaTerm::ByteView data)
    {
        TerminalCore::InputWriteResult result;
        result.requestedBytes = data.size;
        isize offset = 0;
        while (offset < data.size
               && accepting.load(std::memory_order_acquire)) {
            const isize length =
                std::min<isize>(ParserBatchSize, data.size - offset);
            isize queuedBytes = 0;
            if (!bytes.enqueue(NovaTerm::ByteView(data.data + offset, length), 0,
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
        std::unique_lock<std::mutex> locker(commandMutex);
        if (!accepting.load(std::memory_order_acquire))
            return false;
        // 记录入队时刻的已提交字节计数。worker 线程消费时据此等待：
        // 仅当 completedBytes >= byteBarrier 时才执行本命令，从而保证
        // 命令在它之前提交的字节流之后被处理。
        command.byteBarrier = submittedBytes.load(std::memory_order_acquire);
        const isize commandBytes = estimatedCommandBytes(command);

        // 同类型状态命令在队尾合并：只保留最新值，避免连续 resize 或
        // flush 命令在队列中堆积。键盘/鼠标命令不合并（顺序敏感）。
        if ((command.type == CommandType::Resize
             || command.type == CommandType::DefaultColors
             || command.type == CommandType::SetScrollbackLimit
             || command.type == CommandType::Flush
             || command.type == CommandType::PublishContext)
            && !commands.empty()
            && commands.back().type == command.type) {
            pendingCommandBytes -= estimatedCommandBytes(commands.back());
            commands.back() = std::move(command);
            pendingCommandBytes += commandBytes;
            locker.unlock();
            bytes.wakeConsumer();
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
        locker.unlock();
        bytes.wakeConsumer();
        return true;
    }

    static isize estimatedCommandBytes(const ParserCommand& command)
    {
        return isize(sizeof(ParserCommand))
            + command.text.size() * isize(sizeof(QChar));
    }

    bool waitForIdle(int timeoutMs) const
    {
        const uint64_t targetBytes =
            submittedBytes.load(std::memory_order_acquire);
        const uint64_t targetCommands =
            submittedCommands.load(std::memory_order_acquire);
        std::unique_lock<std::mutex> locker(completionMutex);
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeoutMs);
        while (completedBytes.load(std::memory_order_acquire) < targetBytes
               || completedCommands.load(std::memory_order_acquire)
                      < targetCommands) {
            if (completionChanged.wait_until(locker, deadline)
                == std::cv_status::timeout) {
                // 超时后再确认一次，避免恰在截止时刻完成却误报失败。
                if (completedBytes.load(std::memory_order_acquire) < targetBytes
                    || completedCommands.load(std::memory_order_acquire)
                           < targetCommands) {
                    return false;
                }
            }
        }
        return true;
    }

    // worker 线程主循环：libvterm 解析与命令执行均在此串行进行。
    // 每轮先消费已就绪命令（受 byteBarrier 约束），再取一批字节喂给
    // libvterm。模型锁（modelMutex）只在访问 ScreenBuffer/VTAdapter 时
    // 短暂持有，信号发布则脱离锁通过 QueuedConnection 投递到 GUI 线程。
    void workerMain()
    {
        NovaTerm::setCurrentThreadName("nvterm-parser");
        createAdapter();

        while (!stopping.load(std::memory_order_acquire)) {
            const uint64_t processedCommands = processCommands();
            if (processedCommands > 0) {
                completedCommands.fetch_add(processedCommands,
                                            std::memory_order_release);
                notifyCompletion();
            }

            isize queuedAfterTake = 0;
            const isize taken = bytes.take(batchBuffer.data(), ParserBatchSize,
                                           -1, &queuedAfterTake);
            if (taken > 0) {
                // take() 在持锁期间已回报剩余字节数，无需再锁一次 statistics()。
                if (queuedAfterTake <= QueueLowWatermark)
                    setBackpressure(false);
                {
                    std::lock_guard<std::mutex> modelLocker(modelMutex);
                    adapter->writeInput(NovaTerm::ByteView(batchBuffer.data(),
                                                           taken));
                    adapter->flushDamage();
                    commitPendingModelRevision();
                    maybePublishContextLocked();
                }
                publishPendingSignals();
                completedBytes.fetch_add(uint64_t(taken),
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
        observer.output = [this](NovaTerm::ByteView data) {
            constexpr isize OutputBatchSize = 64 * 1024;
            // pendingOutput 仍是 Qt 门面的暂存容器（QByteArray），在此把
            // core 的 ByteView 桥接过去；阶段 7 建门面后这一步收进门面。
            if (pendingOutput.isEmpty()
                || pendingOutput.back().size() + data.size > OutputBatchSize) {
                pendingOutput.push_back(
                    QByteArray(data.data, data.size));
            } else {
                pendingOutput.back().append(data.data, data.size);
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
        observer.titleChanged = [this](const std::string& value) {
            // core 以 UTF-8 std::string 发布标题；门面侧转成 QString 暂存，
            // 阶段 7 建门面后这一步收进门面。
            currentTitle = QString::fromUtf8(value.data(),
                                             qsizetype(value.size()));
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

        std::lock_guard<std::mutex> modelLocker(modelMutex);
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
            std::lock_guard<std::mutex> locker(commandMutex);
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
            std::lock_guard<std::mutex> modelLocker(modelMutex);
            for (const ParserCommand& command : local)
                executeCommand(command);
            commitPendingModelRevision();
            maybePublishContextLocked();
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
        case CommandType::PublishContext:
            break;
        }
    }

    void maybePublishContextLocked()
    {
        if (!contextRequested.exchange(false, std::memory_order_acq_rel))
            return;
        const auto current = std::atomic_load(&publishedContext);
        if (current && current->state.revision == modelRevision) {
            contextReuseCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto published = std::make_shared<NovaTerm::PublishedTerminalState>();
        published->state = owner->terminalStateLocked(
            0, NovaTerm::TerminalState::MaxBytes,
            NovaTerm::TerminalState::MaxLines);
        published->capturedAt = std::chrono::system_clock::now();
        std::atomic_store(&publishedContext,
            std::shared_ptr<const NovaTerm::PublishedTerminalState>(
                std::move(published)));
        contextPublishCount.fetch_add(1, std::memory_order_relaxed);
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
        const u64 revisionValue = std::exchange(pendingRevision, 0);

        if (damageValue.isEmpty() && !cursorValue && !titleValue && !bellValue
            && !scrollbackValue && screenScrollRows == 0 && output.isEmpty()) {
            return;
        }

        QString titleCopy;
        if (titleValue) {
            std::lock_guard<std::mutex> locker(modelMutex);
            titleCopy = currentTitle;
        }

        QMetaObject::invokeMethod(
            owner,
            [target = owner, damageValue = std::move(damageValue),
             revisionValue, cursorValue, titleValue, titleCopy, bellValue,
             scrollbackValue, screenScrollRows,
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
        if (pendingDamage.isEmpty() && !cursorChanged && !scrollbackChanged
            && !titleChanged)
            return;
        pendingRevision = ++modelRevision;
        if (isize(rowRevisions.size()) != screen.rows()) {
            rowRevisions.assign(std::size_t(screen.rows()), modelRevision);
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
        std::lock_guard<std::mutex> locker(completionMutex);
        completionChanged.notify_all();
    }

    TerminalCore* owner;
    mutable std::mutex modelMutex;
    NovaTerm::ScreenBuffer screen;
    ScrollbackBuffer scrollback;
    NovaTerm::CursorState cursor;
    QString currentTitle;
    u64 modelRevision{0};
    u64 pendingRevision{0};
    std::vector<u64> rowRevisions;

    NovaTerm::BoundedByteQueue bytes;
    // worker 线程复用的取批缓冲，避免每批一次堆分配。仅 workerMain 触碰。
    std::vector<char> batchBuffer = std::vector<char>(ParserBatchSize);
    mutable std::mutex commandMutex;
    std::deque<ParserCommand> commands;
    isize pendingCommandBytes{0};

    mutable std::mutex completionMutex;
    mutable std::condition_variable completionChanged;
    std::atomic<uint64_t> submittedBytes{0};
    std::atomic<uint64_t> completedBytes{0};
    std::atomic<uint64_t> submittedCommands{0};
    std::atomic<uint64_t> completedCommands{0};
    std::atomic<bool> accepting{true};
    std::atomic<bool> stopping{false};
    std::shared_ptr<const NovaTerm::PublishedTerminalState> publishedContext;
    std::atomic<bool> contextRequested{false};
    std::atomic<qint64> lastContextRequestNs{0};
    std::atomic<u64> contextRequestCount{0};
    std::atomic<u64> contextPublishCount{0};
    std::atomic<u64> contextReuseCount{0};
    std::atomic<bool> backpressure{false};
    std::thread thread;
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
    return _runtime->enqueueBytes(NovaTerm::ByteView(data.data(), data.size()));
}

void TerminalCore::processKeyPress(QKeyEvent* event)
{
    if (!event)
        return;

    const QString text = event->text();
    const int qtKey = event->key();
    const auto qtModifiers = event->modifiers();
    const int modifiers = int(KeyMapper::modToVTermMod(coreModsFromQt(qtModifiers)));

    const bool hasPrintableText = !text.isEmpty() && text[0].isPrint();
    // Windows 把 AltGr 报成 Ctrl+Alt。AltGr 组合产生可打印字符（如德语键盘
    // AltGr+Q = @、AltGr+8 = [），必须走文本路径；若误入 Ctrl 分支会把
    // 它当成控制字符 —— AltGr+Q 会发出 0x11（XOFF）冻结显示。判据是
    // Ctrl 与 Alt 同时按下且产生了可打印文本。
    const bool likelyAltGr = qtModifiers.testFlag(Qt::ControlModifier)
        && qtModifiers.testFlag(Qt::AltModifier) && hasPrintableText;

    if (qtModifiers.testFlag(Qt::ControlModifier) && !likelyAltGr) {
        uint32_t controlCodepoint = 0;
        if (KeyMapper::codepointToControlCharacter(uint32_t(qtKey),
                                                   controlCodepoint)) {
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

    if (hasPrintableText) {
        processTextInput(text, qtModifiers);
        return;
    }

    VTermKey key;
    if (KeyMapper::keyToVTermKey(coreKeyFromQt(qtKey), key)) {
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
    // 可打印文本已经编码了 Shift（"A" 与 "a" 是不同码点），不能再把 Shift
    // 转发给 libvterm —— 否则空格会因 libvterm 对 Shift+Space 的特判发出
    // CSI 32;2u 而不是 0x20。AltGr（Ctrl+Alt）不是 meta 前缀，其产生的字符
    // 应原样发送。只有单独的 Alt（真正的 meta）才作为 ESC 前缀转发。
    const bool metaAlt = modifiers.testFlag(Qt::AltModifier)
        && !modifiers.testFlag(Qt::ControlModifier);
    const int vtermModifiers = metaAlt ? int(VTERM_MOD_ALT) : int(VTERM_MOD_NONE);
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
    command.first = vtermMouseButton(event->button());
    command.second =
        int(KeyMapper::modToVTermMod(coreModsFromQt(event->modifiers())));
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
    command.first = vtermMouseButton(event->button());
    command.second =
        int(KeyMapper::modToVTermMod(coreModsFromQt(event->modifiers())));
    command.pressed = false;
    _runtime->enqueueCommand(std::move(command));
}

void TerminalCore::processWheel(QWheelEvent* event)
{
    if (!event || event->angleDelta().y() == 0)
        return;
    const int button = event->angleDelta().y() > 0 ? 4 : 5;
    const int modifiers =
        int(KeyMapper::modToVTermMod(coreModsFromQt(event->modifiers())));
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
    std::lock_guard<std::mutex> locker(_runtime->modelMutex);
    return _runtime->screen.columns();
}

int TerminalCore::rows() const
{
    std::lock_guard<std::mutex> locker(_runtime->modelMutex);
    return _runtime->screen.rows();
}

bool TerminalCore::getCell(int row, int col, NovaTerm::Cell& out) const
{
    std::lock_guard<std::mutex> locker(_runtime->modelMutex);
    const NovaTerm::Cell* cell = _runtime->screen.cellAt(row, col);
    if (!cell)
        return false;
    out = *cell;
    return true;
}

bool TerminalCore::rowContinuation(int row) const
{
    std::lock_guard<std::mutex> locker(_runtime->modelMutex);
    return _runtime->screen.rowContinuation(row);
}

NovaTerm::TerminalSnapshot TerminalCore::snapshot() const
{
    std::lock_guard<std::mutex> locker(_runtime->modelMutex);
    NovaTerm::TerminalSnapshot result =
        NovaTerm::makeSnapshot(_runtime->screen, _runtime->cursor);
    result.revision = _runtime->modelRevision;
    return result;
}

NovaTerm::TerminalState TerminalCore::terminalState(
    NovaTerm::u64 sinceLineId, std::size_t maxBytes, std::size_t maxLines) const
{
    std::lock_guard<std::mutex> locker(_runtime->modelMutex);
    return terminalStateLocked(sinceLineId, maxBytes, maxLines);
}

std::optional<NovaTerm::TerminalState> TerminalCore::tryTerminalState(
    NovaTerm::u64 sinceLineId, std::size_t maxBytes, std::size_t maxLines) const
{
    std::unique_lock<std::mutex> locker(_runtime->modelMutex, std::try_to_lock);
    if (!locker.owns_lock())
        return std::nullopt;
    return terminalStateLocked(sinceLineId, maxBytes, maxLines);
}

std::optional<NovaTerm::u64> TerminalCore::tryModelRevision() const
{
    std::unique_lock<std::mutex> locker(_runtime->modelMutex, std::try_to_lock);
    if (!locker.owns_lock())
        return std::nullopt;
    return _runtime->modelRevision;
}

std::shared_ptr<const NovaTerm::PublishedTerminalState>
TerminalCore::requestPublishedTerminalState()
{
    _runtime->contextRequestCount.fetch_add(1, std::memory_order_relaxed);
    const auto current = std::atomic_load(&_runtime->publishedContext);
    if (current)
        _runtime->contextReuseCount.fetch_add(1, std::memory_order_relaxed);

    constexpr qint64 IntervalNs = 250'000'000;
    const qint64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    qint64 previous = _runtime->lastContextRequestNs.load(
        std::memory_order_acquire);
    if ((previous == 0 || now - previous >= IntervalNs)
        && _runtime->lastContextRequestNs.compare_exchange_strong(
            previous, now, std::memory_order_acq_rel)) {
        _runtime->contextRequested.store(true, std::memory_order_release);
        ParserCommand command;
        command.type = CommandType::PublishContext;
        if (!_runtime->enqueueCommand(std::move(command))) {
            _runtime->contextRequested.store(false, std::memory_order_release);
            _runtime->lastContextRequestNs.store(0, std::memory_order_release);
        }
    }
    return current;
}

NovaTerm::PublishedContextStatistics
TerminalCore::publishedContextStatistics() const noexcept
{
    return {
        _runtime->contextRequestCount.load(std::memory_order_relaxed),
        _runtime->contextPublishCount.load(std::memory_order_relaxed),
        _runtime->contextReuseCount.load(std::memory_order_relaxed),
    };
}

NovaTerm::TerminalState TerminalCore::terminalStateLocked(
    NovaTerm::u64 sinceLineId, std::size_t maxBytes, std::size_t maxLines) const
{
    NovaTerm::TerminalState result;
    result.revision = _runtime->modelRevision;
    result.cursor = _runtime->cursor;
    result.alternateScreen = _runtime->adapter->alternateScreen();
    maxBytes = std::min(maxBytes, NovaTerm::TerminalState::MaxBytes);
    maxLines = std::min(maxLines, NovaTerm::TerminalState::MaxLines);
    std::size_t remaining = maxBytes;
    // 只编码解析后的 Unicode scalar；预算边界不切断 UTF-8 字符。
    const auto appendScalar = [&](std::string& text, char32_t cp) {
        if (cp < 32 || (cp >= 127 && cp <= 159)
            || cp == NovaTerm::WideCharContinuation)
            return;
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            cp = 0xfffd;
        char bytes[4];
        int count = 0;
        if (cp < 0x80) bytes[count++] = char(cp);
        else if (cp < 0x800) {
            bytes[count++] = char(0xc0 | (cp >> 6));
            bytes[count++] = char(0x80 | (cp & 63));
        } else if (cp < 0x10000) {
            bytes[count++] = char(0xe0 | (cp >> 12));
            bytes[count++] = char(0x80 | ((cp >> 6) & 63));
            bytes[count++] = char(0x80 | (cp & 63));
        } else {
            bytes[count++] = char(0xf0 | (cp >> 18));
            bytes[count++] = char(0x80 | ((cp >> 12) & 63));
            bytes[count++] = char(0x80 | ((cp >> 6) & 63));
            bytes[count++] = char(0x80 | (cp & 63));
        }
        if (std::size_t(count) <= remaining) {
            text.append(bytes, std::size_t(count));
            remaining -= std::size_t(count);
        } else result.truncated = true;
    };
    const auto appendCell = [&](std::string& text, const NovaTerm::Cell& cell) {
        if (cell.isWideContinuation())
            return;
        if (cell.chars[0] == 0) appendScalar(text, ' ');
        else for (const auto cp : cell.chars) {
            if (cp == 0) break;
            appendScalar(text, cp);
        }
    };
    // 门面中暂存的标题转 Unicode 后按同一字节预算编码，控制字符不进入上下文。
    for (const char32_t cp : _runtime->currentTitle.toUcs4())
        appendScalar(result.title, cp);
    const int rows = _runtime->screen.rows();
    const int columns = _runtime->screen.columns();
    for (int row = 0; row < rows && remaining > 0; ++row) {
        const bool continuation = _runtime->screen.rowContinuation(row);
        if (!continuation || result.viewport.empty()) {
            if (result.viewport.size() >= maxLines) { result.truncated = true; break; }
            result.viewport.push_back({static_cast<NovaTerm::u64>(row), {}});
            // 可见首行可能是历史尾部未完成逻辑行的延续，保留跨接缝前缀。
            if (row == 0 && continuation && !result.alternateScreen) {
                const auto* prefix = _runtime->scrollback.logicalLineAt(
                    _runtime->scrollback.lineCount() - 1);
                if (prefix && !prefix->hardBreak) {
                    // 前缀最多占一半预算，并从尾部取，保留当前屏幕的输出空间。
                    constexpr std::size_t MaxUtf8CellBytes = NovaTerm::MaxCharsPerCell * 4;
                    const auto prefixCells = std::min(prefix->cells.size(),
                        remaining / (2 * MaxUtf8CellBytes));
                    const auto first = prefix->cells.size() - prefixCells;
                    if (first != 0) result.truncated = true;
                    for (auto index = first; index < prefix->cells.size(); ++index) {
                        appendCell(result.viewport.back().text, prefix->cells[index]);
                    }
                }
            }
        }
        auto& text = result.viewport.back().text;
        result.viewport.back().complete = row < result.cursor.position.row;
        int end = columns;
        if (row + 1 == rows || !_runtime->screen.rowContinuation(row + 1)) {
            while (end > 0) {
                const auto* cell = _runtime->screen.cellAt(row, end - 1);
                if (cell->chars[0] != 0 && cell->chars[0] != ' ') break;
                --end;
            }
        }
        for (int col = 0; col < end && remaining > 0; ++col)
            appendCell(text, *_runtime->screen.cellAt(row, col));
    }
    if (!result.alternateScreen) {
        const auto count = _runtime->scrollback.lineCount();
        const auto first = std::max<NovaTerm::isize>(0, count - NovaTerm::isize(maxLines));
        if (first > 0) {
            const auto* preceding = _runtime->scrollback.logicalLineAt(first - 1);
            if (preceding && preceding->id > sinceLineId)
                result.truncated = true;
        }
        for (NovaTerm::isize index = count - 1; index >= first && remaining > 0; --index) {
            const auto* line = _runtime->scrollback.logicalLineAt(index);
            if (!line || line->id <= sinceLineId || !line->hardBreak) continue;
            if (result.recentOutput.size() + result.viewport.size() >= maxLines) {
                result.truncated = true;
                break;
            }
            result.recentOutput.push_back({line->id, {}});
            for (const auto& cell : line->cells) {
                appendCell(result.recentOutput.back().text, cell);
                if (remaining == 0) break;
            }
        }
        std::reverse(result.recentOutput.begin(), result.recentOutput.end());
    }
    if (remaining == 0)
        result.truncated = true;
    return result;
}

NovaTerm::RendererSnapshot TerminalCore::rendererSnapshot(
    const std::vector<bool>& dirtyRows, int scrollLine,
    NovaTerm::LineId anchorLine, isize anchorWrap,
    u64 rendererContentRevision) const
{
    std::lock_guard<std::mutex> locker(_runtime->modelMutex);
    NovaTerm::RendererSnapshot snapshot;
    snapshot.revision = _runtime->modelRevision;
    snapshot.columns = _runtime->screen.columns();
    snapshot.rows = _runtime->screen.rows();
    snapshot.cursor = _runtime->cursor;
    snapshot.visibleRowRevisions.resize(std::size_t(snapshot.rows));
    snapshot.visibleRowIdentities.resize(std::size_t(snapshot.rows));
    snapshot.visibleRowBlockIdentities.resize(std::size_t(snapshot.rows));
    snapshot.visibleRows.resize(std::size_t(snapshot.rows));

    // dirtyRows 与当前行数不一致时（窗口刚 resize 过），无法按位判断
    // 脏行，只能强制全量拷贝所有行。
    const bool copyAllRows = isize(dirtyRows.size()) != snapshot.rows;
    NovaTerm::ScrollbackSnapshot history;
    NovaTerm::ViewportSnapshot historyViewport;
    if (scrollLine > 0) {
        history = _runtime->scrollback.snapshot();
    }
    // 滚动回看时按 anchorLine+anchorWrap 或末尾行 + scrollLine 计算视口。
    // LineLayout::viewport 负责把逻辑行按当前列宽重新折行，输出
    // 每个可见 widget 行对应的逻辑行 ID 及 Cell 切片范围。
    if (!history.empty()) {
        // anchorLine 非 0 时优先按 ID 定位；但锚点行可能已被淘汰出历史
        // （回看期间持续输出、scrollback 达上限）。此时 lineById 返回
        // nullptr，若不兜底会导致 historyViewport 为空、整个回看区渲染成
        // 空白。回退到"末尾行 + scrollLine"（与 anchorLine==0 同路径），
        // 保证有历史时回看区始终有内容。
        const NovaTerm::LogicalLine* anchor =
            anchorLine != 0 ? history.lineById(anchorLine) : nullptr;
        isize effectiveWrap = anchorWrap;
        if (!anchor) {
            anchor = history.lineAt(
                std::max<isize>(0, history.lineCount() - scrollLine));
            effectiveWrap = 0;
        }
        if (anchor) {
            historyViewport = NovaTerm::LineLayout::viewport(
                history, anchor->id, effectiveWrap, snapshot.columns,
                std::min<isize>(scrollLine, snapshot.rows), 0,
                history.version());
        }
    }
    // widgetRow 是渲染器视角下的行号；screenRow 是它在活动屏幕中的位置。
    // screenRow < 0 表示该行落在 scrollback 中，需要从 historyViewport 取。
    for (int widgetRow = 0; widgetRow < snapshot.rows; ++widgetRow) {
        const int screenRow = widgetRow - scrollLine;
        snapshot.visibleRowRevisions[widgetRow] = screenRow >= 0
            && screenRow < isize(_runtime->rowRevisions.size())
            ? _runtime->rowRevisions[screenRow]
            : _runtime->modelRevision;
        // 渲染器声明该行未脏：只回填身份哈希，跳过 Cell 拷贝。
        if (!copyAllRows && !dirtyRows[widgetRow]) {
            if (screenRow >= 0) {
                // revision 补回优化：渲染器只在 row revision 严格大于它已消费的
                // rendererContentRevision 时才读该行指纹（否则走 revision 分支
                // 直接跳过）。因此 revision 不超过该值的非脏行，指纹算了也无人读，
                // 填 0 即可，省去整行哈希。rendererContentRevision==0（默认或
                // live-scroll 全比对路径）时退化为对所有非脏行计算指纹。
                const u64 rowRevision =
                    snapshot.visibleRowRevisions[widgetRow];
                if (rowRevision > rendererContentRevision) {
                    snapshot.visibleRowIdentities[widgetRow] =
                        rowContentIdentity(
                            _runtime->screen.cellAt(screenRow, 0),
                            snapshot.columns, snapshot.columns);
                }
            } else if (widgetRow < isize(historyViewport.rows.size())) {
                // 历史行同样要有真实 identity（不能恒为 0），否则脏帧算出的
                // 哈希与非脏帧的 0 不一致，会被渲染器误判为内容变化而多余重建。
                // 直接哈希历史切片（count = 切片长，补白隐含），零分配。
                const auto& display = historyViewport.rows[widgetRow];
                const auto* logical = history.lineById(display.lineId);
                if (logical) {
                    const isize count = std::min<isize>(
                        snapshot.columns, display.endCell - display.startCell);
                    snapshot.visibleRowIdentities[widgetRow] =
                        rowContentIdentity(
                            logical->cells.data() + display.startCell,
                            int(std::max<isize>(0, count)), snapshot.columns);
                }
            }
            continue;
        }
        std::vector<NovaTerm::Cell> destination;
        destination.resize(snapshot.columns);
        // 该行位于 scrollback 视口：从对应的逻辑行切片拷贝。
        if (screenRow < 0) {
            if (widgetRow < isize(historyViewport.rows.size())) {
                const auto& display = historyViewport.rows[widgetRow];
                const auto* logical = history.lineById(display.lineId);
                if (logical) {
                    const isize count = std::min<isize>(
                        snapshot.columns, display.endCell - display.startCell);
                    std::copy_n(logical->cells.cbegin() + display.startCell,
                                count, destination.begin());
                }
            }
            snapshot.visibleRowIdentities[widgetRow] =
                rowContentIdentity(destination.data(),
                                   int(destination.size()),
                                   int(destination.size()),
                                   &snapshot.visibleRowBlockIdentities[
                                       std::size_t(widgetRow)]);
            snapshot.visibleRows[widgetRow] =
                std::make_shared<const std::vector<NovaTerm::Cell>>(
                    std::move(destination));
            continue;
        }
        // 该行位于活动屏幕：直接整行拷贝。
        const NovaTerm::Cell* source = _runtime->screen.cellAt(screenRow, 0);
        if (source)
            std::copy_n(source, snapshot.columns, destination.begin());
        snapshot.visibleRowIdentities[widgetRow] =
            rowContentIdentity(destination.data(), int(destination.size()),
                               int(destination.size()),
                               &snapshot.visibleRowBlockIdentities[
                                   std::size_t(widgetRow)]);
        snapshot.visibleRows[widgetRow] =
            std::make_shared<const std::vector<NovaTerm::Cell>>(
                std::move(destination));
    }
    return snapshot;
}

u64 TerminalCore::modelRevision() const
{
    std::lock_guard<std::mutex> locker(_runtime->modelMutex);
    return _runtime->modelRevision;
}

NovaTerm::CursorState TerminalCore::cursorState() const
{
    std::lock_guard<std::mutex> locker(_runtime->modelMutex);
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
    std::lock_guard<std::mutex> locker(_runtime->modelMutex);
    return _runtime->scrollback.lineCount();
}

bool TerminalCore::getScrollbackCell(int lineIndex, int col,
                                     NovaTerm::Cell& out) const
{
    std::lock_guard<std::mutex> locker(_runtime->modelMutex);
    const auto* line = _runtime->scrollback.lineVectorAt(lineIndex);
    if (!line || col < 0 || col >= _runtime->scrollback.columns())
        return false;
    out = col < isize(line->size()) ? line->at(col) : NovaTerm::Cell{};
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
    std::lock_guard<std::mutex> locker(_runtime->modelMutex);
    return _runtime->scrollback.snapshot();
}

NovaTerm::ScrollbackTail TerminalCore::scrollbackTail(
    NovaTerm::LineId sinceId, isize maxLines) const
{
    NovaTerm::ScrollbackTail tail;
    std::lock_guard<std::mutex> locker(_runtime->modelMutex);
    _runtime->scrollback.tailFrom(sinceId, maxLines, tail);
    return tail;
}

NovaTerm::ScrollbackStatistics TerminalCore::scrollbackStatistics() const
{
    std::lock_guard<std::mutex> locker(_runtime->modelMutex);
    return _runtime->scrollback.statistics();
}

void TerminalCore::searchScrollback(NovaTerm::SearchRequest request)
{
    _searchEngine->search(scrollbackSnapshot(), std::move(request));
}

void TerminalCore::cancelSearch(u64 generation)
{
    _searchEngine->cancel(generation);
}

void TerminalCore::requestScrollbackReflow(int columns, u64 generation,
                                           isize batchLines)
{
    _reflowEngine->request(scrollbackSnapshot(), columns, generation,
                           batchLines);
}

void TerminalCore::cancelScrollbackReflow(u64 generation)
{
    _reflowEngine->cancel(generation);
}

NovaTerm::Position TerminalCore::cursorPosition() const
{
    std::lock_guard<std::mutex> locker(_runtime->modelMutex);
    return _runtime->cursor.position;
}

bool TerminalCore::cursorVisible() const
{
    std::lock_guard<std::mutex> locker(_runtime->modelMutex);
    return _runtime->cursor.visible;
}

NovaTerm::CursorShape TerminalCore::cursorShape() const
{
    std::lock_guard<std::mutex> locker(_runtime->modelMutex);
    return _runtime->cursor.shape;
}

bool TerminalCore::cursorBlink() const
{
    std::lock_guard<std::mutex> locker(_runtime->modelMutex);
    return _runtime->cursor.blink;
}

QString TerminalCore::title() const
{
    std::lock_guard<std::mutex> locker(_runtime->modelMutex);
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
