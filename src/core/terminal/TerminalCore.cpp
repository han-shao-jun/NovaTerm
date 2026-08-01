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

constexpr qsizetype QueueCapacity = 8 * 1024 * 1024;
constexpr qsizetype ParserBatchSize = 64 * 1024;
constexpr qsizetype QueueHighWatermark = QueueCapacity * 3 / 4;
constexpr qsizetype QueueLowWatermark = QueueCapacity / 2;
constexpr size_t MaximumPendingCommands = 4096;
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
    uint64_t byteBarrier{0};
};

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
        command.byteBarrier = submittedBytes.load(std::memory_order_acquire);
        const qsizetype commandBytes =
            qsizetype(sizeof(ParserCommand))
            + command.text.size() * qsizetype(sizeof(QChar));

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
        observer.output = [this](const QByteArray& data) {
            pendingOutput.push_back(data);
        };
        observer.damage = [this](const NovaTerm::DirtyRegion& region) {
            // Preserve sparse regions until RenderScheduler can merge them.
            // A single bounding box makes distant cell changes look like a
            // near-full-screen update.
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

        QMutexLocker modelLocker(&modelMutex);
        adapter = std::make_unique<NovaTerm::VTAdapter>(
            screen.columns(), screen.rows(), screen, scrollback,
            std::move(observer));
    }

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

    void publishPendingSignals()
    {
        QVector<NovaTerm::DirtyRegion> damageValue;
        damageValue.swap(pendingDamage);
        const bool cursorValue = std::exchange(cursorChanged, false);
        const bool titleValue = std::exchange(titleChanged, false);
        const bool bellValue = std::exchange(bellPending, false);
        const bool scrollbackValue = std::exchange(scrollbackChanged, false);
        QVector<QByteArray> output;
        output.swap(pendingOutput);

        if (damageValue.isEmpty() && !cursorValue && !titleValue && !bellValue
            && !scrollbackValue && output.isEmpty()) {
            return;
        }

        QString titleCopy;
        if (titleValue) {
            QMutexLocker locker(&modelMutex);
            titleCopy = currentTitle;
        }

        QMetaObject::invokeMethod(
            owner,
            [target = owner, damageValue, cursorValue, titleValue, titleCopy,
             bellValue, scrollbackValue, output = std::move(output)]() {
                for (const QByteArray& data : output)
                    emit target->outputData(data);
                for (const NovaTerm::DirtyRegion& region : damageValue)
                    emit target->damage(region);
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
            // The codepoint is already controlled. Retain Alt/Shift so
            // libvterm can add their terminal semantics, but do not apply Ctrl
            // a second time.
            command.first = modifiers & ~int(VTERM_MOD_CTRL);
            _runtime->enqueueCommand(std::move(command));
            return;
        }
    }

    if (!text.isEmpty() && text[0].isPrint()) {
        for (const QChar& character : text) {
            ParserCommand command;
            command.type = CommandType::KeyboardCharacter;
            command.codepoint = character.unicode();
            command.first = modifiers;
            _runtime->enqueueCommand(std::move(command));
        }
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

    for (const QChar& character : text) {
        ParserCommand command;
        command.type = CommandType::KeyboardCharacter;
        command.codepoint = character.unicode();
        command.first = modifiers;
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
    ParserCommand command;
    command.type = CommandType::Paste;
    command.text = text;
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
    return NovaTerm::makeSnapshot(_runtime->screen, _runtime->cursor);
}

NovaTerm::RendererSnapshot TerminalCore::rendererSnapshot(
    const QVector<bool>& dirtyRows, int scrollLine,
    NovaTerm::LineId anchorLine, qsizetype anchorWrap) const
{
    QMutexLocker locker(&_runtime->modelMutex);
    NovaTerm::RendererSnapshot snapshot;
    snapshot.columns = _runtime->screen.columns();
    snapshot.rows = _runtime->screen.rows();
    snapshot.cursor = _runtime->cursor;
    snapshot.visibleRows.resize(snapshot.rows);

    const bool copyAllRows = dirtyRows.size() != snapshot.rows;
    NovaTerm::ScrollbackSnapshot history;
    NovaTerm::ViewportSnapshot historyViewport;
    if (scrollLine > 0) {
        history = _runtime->scrollback.snapshot();
    }
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
    for (int widgetRow = 0; widgetRow < snapshot.rows; ++widgetRow) {
        if (!copyAllRows && !dirtyRows[widgetRow])
            continue;
        QVector<NovaTerm::Cell>& destination = snapshot.visibleRows[widgetRow];
        destination.resize(snapshot.columns);
        const int screenRow = widgetRow - scrollLine;
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
            continue;
        }
        const NovaTerm::Cell* source = _runtime->screen.cellAt(screenRow, 0);
        if (source)
            std::copy_n(source, snapshot.columns, destination.begin());
    }
    return snapshot;
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
