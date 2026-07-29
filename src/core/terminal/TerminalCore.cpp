#include "TerminalCore.h"

#include "KeyMapper.h"
#include "VTAdapter.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

TerminalCore::TerminalCore(int cols, int rows, QObject* parent)
    : QObject(parent)
    , _screen(cols, rows)
    , _scrollback(1000)
{
    NovaTerm::VTAdapter::Observer observer;
    observer.output = [this](const QByteArray& data) {
        emit outputData(data);
    };
    observer.damage = [this](const NovaTerm::DirtyRegion& region) {
        emit damage(region);
    };
    observer.cursorChanged = [this](const NovaTerm::CursorState& cursor) {
        _cursor = cursor;
        emit cursorMoved();
    };
    observer.titleChanged = [this](const QString& title) {
        _title = title;
        emit titleChanged(title);
    };
    observer.bell = [this]() {
        emit bell();
    };
    observer.scrollbackChanged = [this]() {
        emit scrollbackChanged();
    };

    _adapter = std::make_unique<NovaTerm::VTAdapter>(
        cols, rows, _screen, _scrollback, std::move(observer));
}

TerminalCore::~TerminalCore() = default;

void TerminalCore::writeInput(const QByteArray& data)
{
    if (_adapter) {
        _adapter->writeInput(data);
        _adapter->flushDamage();
    }
}

void TerminalCore::processKeyPress(QKeyEvent* event)
{
    if (!_adapter || !event)
        return;

    const QString text = event->text();
    const int qtKey = event->key();
    const auto qtModifiers = event->modifiers();
    const auto vtModifiers = KeyMapper::qtModToVTermMod(qtModifiers);

    if (!text.isEmpty() && text[0].isPrint()) {
        for (const QChar& character : text)
            _adapter->keyboardUnichar(character.unicode(), int(vtModifiers));
        return;
    }

    VTermKey key;
    if (KeyMapper::qtKeyToVTermKey(qtKey, key)) {
        _adapter->keyboardKey(int(key), int(vtModifiers));
        return;
    }

    if (qtModifiers & Qt::KeypadModifier) {
        if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9) {
            const VTermKey keypadKeys[] = {
                VTERM_KEY_KP_0, VTERM_KEY_KP_1, VTERM_KEY_KP_2,
                VTERM_KEY_KP_3, VTERM_KEY_KP_4, VTERM_KEY_KP_5,
                VTERM_KEY_KP_6, VTERM_KEY_KP_7, VTERM_KEY_KP_8,
                VTERM_KEY_KP_9
            };
            _adapter->keyboardKey(int(keypadKeys[qtKey - Qt::Key_0]),
                                  int(vtModifiers));
            return;
        }
        if (qtKey == Qt::Key_Period) {
            _adapter->keyboardKey(int(VTERM_KEY_KP_PERIOD), int(vtModifiers));
            return;
        }
        if (qtKey == Qt::Key_Enter || qtKey == Qt::Key_Return) {
            _adapter->keyboardKey(int(VTERM_KEY_KP_ENTER), int(vtModifiers));
            return;
        }
    }

    if (qtModifiers & Qt::ControlModifier && !text.isEmpty()) {
        const uint32_t codepoint = text[0].unicode();
        if (codepoint >= 0x40 && codepoint <= 0x5F) {
            _adapter->keyboardUnichar(codepoint - 0x40, VTERM_MOD_NONE);
            return;
        }
        if (codepoint >= 0x61 && codepoint <= 0x7A) {
            _adapter->keyboardUnichar(codepoint - 0x60, VTERM_MOD_NONE);
            return;
        }
    }

    for (const QChar& character : text)
        _adapter->keyboardUnichar(character.unicode(), int(vtModifiers));
}

void TerminalCore::processMousePress(QMouseEvent* event)
{
    if (!_adapter || !event)
        return;
    const int button = event->button() == Qt::RightButton ? 2
        : event->button() == Qt::MiddleButton ? 3 : 1;
    _adapter->mouseButton(
        button, true, int(KeyMapper::qtModToVTermMod(event->modifiers())));
}

void TerminalCore::processMouseMove(QMouseEvent* event)
{
    Q_UNUSED(event);
}

void TerminalCore::processMouseRelease(QMouseEvent* event)
{
    if (!_adapter || !event)
        return;
    const int button = event->button() == Qt::RightButton ? 2
        : event->button() == Qt::MiddleButton ? 3 : 1;
    _adapter->mouseButton(
        button, false, int(KeyMapper::qtModToVTermMod(event->modifiers())));
}

void TerminalCore::processWheel(QWheelEvent* event)
{
    if (!_adapter || !event)
        return;
    const int delta = event->angleDelta().y();
    if (delta == 0)
        return;

    const int button = delta > 0 ? 4 : 5;
    const int modifiers =
        int(KeyMapper::qtModToVTermMod(event->modifiers()));
    _adapter->mouseButton(button, true, modifiers);
    _adapter->mouseButton(button, false, modifiers);
}

void TerminalCore::focusIn()
{
    if (_adapter)
        _adapter->focusIn();
}

void TerminalCore::focusOut()
{
    if (_adapter)
        _adapter->focusOut();
}

void TerminalCore::pasteText(const QString& text)
{
    if (!_adapter || text.isEmpty())
        return;

    _adapter->startPaste();
    for (const uint32_t codepoint : text.toUcs4())
        _adapter->keyboardUnichar(codepoint, VTERM_MOD_NONE);
    _adapter->endPaste();
}

void TerminalCore::resize(int cols, int rows)
{
    if (!_adapter || cols < 2 || rows < 1
        || (cols == columns() && rows == this->rows())) {
        return;
    }

    _adapter->resize(cols, rows);
}

bool TerminalCore::getCell(int row, int col, NovaTerm::Cell& out) const
{
    const NovaTerm::Cell* cell = _screen.cellAt(row, col);
    if (!cell)
        return false;
    out = *cell;
    return true;
}

NovaTerm::TerminalSnapshot TerminalCore::snapshot() const
{
    return NovaTerm::makeSnapshot(_screen, _cursor);
}

void TerminalCore::flushDamage()
{
    if (_adapter)
        _adapter->flushDamage();
}

void TerminalCore::setDefaultColors(
    const NovaTerm::TerminalColor& foreground,
    const NovaTerm::TerminalColor& background)
{
    if (_adapter)
        _adapter->setDefaultColors(foreground, background);
}

int TerminalCore::scrollbackLineCount() const
{
    return _scrollback.lineCount();
}

bool TerminalCore::getScrollbackCell(int lineIndex, int col,
                                     NovaTerm::Cell& out) const
{
    const auto* line = _scrollback.lineAt(lineIndex);
    if (!line || col < 0 || col >= _scrollback.columns())
        return false;
    out = line[col];
    return true;
}

void TerminalCore::setScrollbackLimit(int lines)
{
    _scrollback.setMaxLines(lines);
}

void TerminalCore::clearScrollback()
{
    _scrollback.clear();
    emit scrollbackChanged();
}
