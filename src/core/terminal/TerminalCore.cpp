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
#include <optional>
#include <utility>
#include <vector>

namespace {

constexpr qsizetype QueueCapacity = 8 * 1024 * 1024;
constexpr qsizetype ParserBatchSize = 64 * 1024;
constexpr size_t MaximumPendingCommands = 4096;

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
};

NovaTerm::DirtyRegion mergedRegion(const NovaTerm::DirtyRegion& first,
                                   const NovaTerm::DirtyRegion& second)
{
    return {std::min(first.startRow, second.startRow),
            std::max(first.endRow, second.endRow),
            std::min(first.startColumn, second.startColumn),
            std::max(first.endColumn, second.endColumn)};
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
        thread = QThread::create([this]() { workerMain(); });
        thread->setObjectName(QStringLiteral("NovaTerm Parser Worker"));
        thread->start();
    }

    ~Runtime()
    {
        stopping.store(true, std::memory_order_release);
        bytes.stop();
        {
            QMutexLocker locker(&commandMutex);
            commands.clear();
        }
        if (thread) {
            thread->wait();
            delete thread;
        }
    }

    void enqueueBytes(const QByteArray& data)
    {
        qsizetype offset = 0;
        while (offset < data.size()
               && !stopping.load(std::memory_order_acquire)) {
            const qsizetype length =
                std::min<qsizetype>(ParserBatchSize, data.size() - offset);
            if (!bytes.enqueue(data.mid(offset, length)))
                return;
            submittedBytes.fetch_add(uint64_t(length),
                                     std::memory_order_release);
            offset += length;
        }
    }

    bool enqueueCommand(ParserCommand command)
    {
        QMutexLocker locker(&commandMutex);
        if (stopping.load(std::memory_order_acquire)
            || commands.size() >= MaximumPendingCommands) {
            return false;
        }
        commands.push_back(std::move(command));
        submittedCommands.fetch_add(1, std::memory_order_release);
        return true;
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
        notifyCompletion();
    }

    void createAdapter()
    {
        NovaTerm::VTAdapter::Observer observer;
        observer.output = [this](const QByteArray& data) {
            pendingOutput.push_back(data);
        };
        observer.damage = [this](const NovaTerm::DirtyRegion& region) {
            pendingDamage = pendingDamage
                ? mergedRegion(*pendingDamage, region) : region;
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
            local.swap(commands);
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
        const auto damageValue = pendingDamage;
        pendingDamage.reset();
        const bool cursorValue = std::exchange(cursorChanged, false);
        const bool titleValue = std::exchange(titleChanged, false);
        const bool bellValue = std::exchange(bellPending, false);
        const bool scrollbackValue = std::exchange(scrollbackChanged, false);
        QVector<QByteArray> output;
        output.swap(pendingOutput);

        if (!damageValue && !cursorValue && !titleValue && !bellValue
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
                if (damageValue)
                    emit target->damage(*damageValue);
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

    mutable QMutex completionMutex;
    mutable QWaitCondition completionChanged;
    std::atomic<uint64_t> submittedBytes{0};
    std::atomic<uint64_t> completedBytes{0};
    std::atomic<uint64_t> submittedCommands{0};
    std::atomic<uint64_t> completedCommands{0};
    std::atomic<bool> stopping{false};
    QThread* thread{nullptr};
    std::unique_ptr<NovaTerm::VTAdapter> adapter;

    std::optional<NovaTerm::DirtyRegion> pendingDamage;
    QVector<QByteArray> pendingOutput;
    bool cursorChanged{false};
    bool titleChanged{false};
    bool bellPending{false};
    bool scrollbackChanged{false};
};

TerminalCore::TerminalCore(int cols, int rows, QObject* parent)
    : QObject(parent)
    , _runtime(std::make_unique<Runtime>(this, cols, rows))
{
}

TerminalCore::~TerminalCore() = default;

void TerminalCore::writeInput(const QByteArray& data)
{
    if (!data.isEmpty())
        _runtime->enqueueBytes(data);
}

void TerminalCore::processKeyPress(QKeyEvent* event)
{
    if (!event)
        return;

    const QString text = event->text();
    const int qtKey = event->key();
    const auto qtModifiers = event->modifiers();
    const int modifiers = int(KeyMapper::qtModToVTermMod(qtModifiers));

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

    if (qtModifiers & Qt::ControlModifier && !text.isEmpty()) {
        const uint32_t codepoint = text[0].unicode();
        if ((codepoint >= 0x40 && codepoint <= 0x5F)
            || (codepoint >= 0x61 && codepoint <= 0x7A)) {
            ParserCommand command;
            command.type = CommandType::KeyboardCharacter;
            command.codepoint = codepoint >= 0x61 ? codepoint - 0x60
                                                  : codepoint - 0x40;
            _runtime->enqueueCommand(std::move(command));
            return;
        }
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
    const auto* line = _runtime->scrollback.lineAt(lineIndex);
    if (!line || col < 0 || col >= _runtime->scrollback.columns())
        return false;
    out = line[col];
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
