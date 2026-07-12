#include "TerminalCore.h"
#include "KeyMapper.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QDebug>
#include <cstring>

// ═══════════════════════════════════════════════════════════════════
//  构造 / 析构
// ═══════════════════════════════════════════════════════════════════

TerminalCore::TerminalCore(int cols, int rows, QObject* parent)
    : QObject(parent)
    , _cols(cols)
    , _rows(rows)
    , _scrollback(1000)
{
    // ── 创建 VTerm ────────────────────────────────────────────
    _vt = vterm_new(rows, cols);
    if (!_vt) {
        qCritical() << "TerminalCore: vterm_new() failed";
        return;
    }

    _vts   = vterm_obtain_screen(_vt);
    _state = vterm_obtain_state(_vt);

    // ── 初始化状态 ────────────────────────────────────────────
    // 必须调用 vterm_state_reset()：
    //   vterm_state_new() 不会初始化 encoding[0..3].enc 指针，
    //   只有 vterm_state_reset() 中的 for(i=0; i<4) 循环会通过
    //   vterm_lookup_encoding() 设置这些指针。不调用 reset 的话
    //   encoding->enc 为 NULL，第一次 on_text 回调即 SIGSEGV。
    vterm_state_reset(_state, 0);
    vterm_set_utf8(_vt, 1);
    vterm_screen_enable_altscreen(_vts, 1);
    vterm_screen_set_damage_merge(_vts, VTERM_DAMAGE_SCROLL);

    // ── 设置输出回调（键盘 → 转义序列） ──────────────────────
    vterm_output_set_callback(_vt, &TerminalCore::onOutput, this);

    // ── 设置 Screen 回调（必须用成员变量，libvterm 只存指针不拷贝）──
    std::memset(&_screenCallbacks, 0, sizeof(_screenCallbacks));
    _screenCallbacks.damage      = &TerminalCore::onDamage;
    _screenCallbacks.moverect    = &TerminalCore::onMoverect;
    _screenCallbacks.movecursor  = &TerminalCore::onMovecursor;
    _screenCallbacks.settermprop = &TerminalCore::onSetTermProp;
    _screenCallbacks.bell        = &TerminalCore::onBell;
    _screenCallbacks.resize      = &TerminalCore::onResize;
    _screenCallbacks.sb_pushline = &TerminalCore::onSbPushLine;
    _screenCallbacks.sb_popline  = &TerminalCore::onSbPopLine;
    _screenCallbacks.sb_clear    = &TerminalCore::onSbClear;
    vterm_screen_set_callbacks(_vts, &_screenCallbacks, this);
}

TerminalCore::~TerminalCore()
{
    if (_vt) {
        vterm_free(_vt);
        _vt = nullptr;
    }
}

// ═══════════════════════════════════════════════════════════════════
//  输入 (PTY/Transport → libvterm)
// ═══════════════════════════════════════════════════════════════════

void TerminalCore::writeInput(const QByteArray& data)
{
    if (!_vt) return;
    vterm_input_write(_vt, data.constData(), data.size());
}

// ═══════════════════════════════════════════════════════════════════
//  键盘处理
// ═══════════════════════════════════════════════════════════════════

void TerminalCore::processKeyPress(QKeyEvent* event)
{
    if (!_vt) return;

    const QString text = event->text();
    const int qtKey    = event->key();
    const auto qtMod   = event->modifiers();
    const auto vmod    = KeyMapper::qtModToVTermMod(qtMod);

    // ── 可打印字符：通过 vterm_keyboard_unichar ───────────────
    // 排除纯修饰键和无文本的控制键
    if (!text.isEmpty() && text[0].isPrint()) {
        for (const QChar& ch : text) {
            vterm_keyboard_unichar(_vt, ch.unicode(), vmod);
        }
        return;
    }

    // ── 特殊键：Qt::Key → VTermKey → vterm_keyboard_key ──────
    VTermKey vk;
    if (KeyMapper::qtKeyToVTermKey(qtKey, vk)) {
        vterm_keyboard_key(_vt, vk, vmod);
        return;
    }

    // ── 小键盘（Qt 中 keypad 数字键 = 对应数字键 + KeypadModifier）──
    if (qtMod & Qt::KeypadModifier) {
        const int baseKey = qtKey;
        if (baseKey >= Qt::Key_0 && baseKey <= Qt::Key_9) {
            const VTermKey kpKeys[] = {
                VTERM_KEY_KP_0, VTERM_KEY_KP_1, VTERM_KEY_KP_2,
                VTERM_KEY_KP_3, VTERM_KEY_KP_4, VTERM_KEY_KP_5,
                VTERM_KEY_KP_6, VTERM_KEY_KP_7, VTERM_KEY_KP_8,
                VTERM_KEY_KP_9
            };
            vterm_keyboard_key(_vt, kpKeys[baseKey - Qt::Key_0], vmod);
            return;
        }
        if (baseKey == Qt::Key_Period) {
            vterm_keyboard_key(_vt, VTERM_KEY_KP_PERIOD, vmod);
            return;
        }
        if (baseKey == Qt::Key_Enter || baseKey == Qt::Key_Return) {
            vterm_keyboard_key(_vt, VTERM_KEY_KP_ENTER, vmod);
            return;
        }
    }

    // ── 控制字符：Ctrl+字母 → ASCII 控制码（1-26） ──────────
    if (qtMod & Qt::ControlModifier && !text.isEmpty()) {
        const uint32_t cp = text[0].unicode();
        if (cp >= 0x40 && cp <= 0x5F) {
            // @ A-Z [ \ ] ^ _ → 0x00-0x1F
            vterm_keyboard_unichar(_vt, cp - 0x40, VTERM_MOD_NONE);
            return;
        }
        if (cp >= 0x61 && cp <= 0x7A) {
            // a-z → 0x01-0x1A
            vterm_keyboard_unichar(_vt, cp - 0x60, VTERM_MOD_NONE);
            return;
        }
    }

    // ── 回退：空格键等 ───────────────────────────────────────
    if (!text.isEmpty()) {
        for (const QChar& ch : text) {
            vterm_keyboard_unichar(_vt, ch.unicode(), vmod);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  鼠标处理
// ═══════════════════════════════════════════════════════════════════

void TerminalCore::processMousePress(QMouseEvent* event)
{
    if (!_vt) return;
    const auto vmod = KeyMapper::qtModToVTermMod(event->modifiers());
    const int button = (event->button() == Qt::RightButton)  ? 2
                     : (event->button() == Qt::MiddleButton) ? 3
                     : 1;  // Left or other → button 1
    vterm_mouse_button(_vt, button, true, vmod);
}

void TerminalCore::processMouseMove(QMouseEvent* event)
{
    // Mouse move events are handled by the renderer to compute row/col.
    // The renderer should call vterm_mouse_move directly if needed.
    Q_UNUSED(event);
}

void TerminalCore::processMouseRelease(QMouseEvent* event)
{
    if (!_vt) return;
    const auto vmod = KeyMapper::qtModToVTermMod(event->modifiers());
    const int button = (event->button() == Qt::RightButton)  ? 2
                     : (event->button() == Qt::MiddleButton) ? 3
                     : 1;
    vterm_mouse_button(_vt, button, false, vmod);
}

void TerminalCore::processWheel(QWheelEvent* event)
{
    // libvterm 用 vterm_mouse_button(button=4/5) 表示滚轮
    if (!_vt) return;
    const auto vmod = KeyMapper::qtModToVTermMod(event->modifiers());
    const int delta = event->angleDelta().y();
    if (delta != 0) {
        const int button = (delta > 0) ? 4 : 5;
        vterm_mouse_button(_vt, button, true, vmod);
        vterm_mouse_button(_vt, button, false, vmod);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  粘贴
// ═══════════════════════════════════════════════════════════════════

void TerminalCore::pasteText(const QString& text)
{
    if (!_vt || text.isEmpty()) return;
    vterm_keyboard_start_paste(_vt);
    vterm_keyboard_unichar(_vt, text.toStdU32String()[0], VTERM_MOD_NONE); // TODO: proper text iteration
    // Simple approach: write each character
    const auto u32 = text.toUcs4();
    for (const uint32_t cp : u32) {
        vterm_keyboard_unichar(_vt, cp, VTERM_MOD_NONE);
    }
    vterm_keyboard_end_paste(_vt);
}

// ═══════════════════════════════════════════════════════════════════
//  尺寸
// ═══════════════════════════════════════════════════════════════════

void TerminalCore::resize(int cols, int rows)
{
    if (cols < 2 || rows < 1) return;
    if (cols == _cols && rows == _rows) return;

    _cols = cols;
    _rows = rows;
    vterm_set_size(_vt, rows, cols);
}

// ═══════════════════════════════════════════════════════════════════
//  屏幕访问
// ═══════════════════════════════════════════════════════════════════

bool TerminalCore::getCell(int row, int col, VTermScreenCell& out) const
{
    if (!_vts || row < 0 || row >= _rows || col < 0 || col >= _cols)
        return false;
    VTermPos pos = { row, col };
    return vterm_screen_get_cell(_vts, pos, &out) != 0;
}

void TerminalCore::flushDamage()
{
    if (_vts)
        vterm_screen_flush_damage(_vts);
}

// ═══════════════════════════════════════════════════════════════════
//  Scrollback
// ═══════════════════════════════════════════════════════════════════

int TerminalCore::scrollbackLineCount() const
{
    return _scrollback.lineCount();
}

bool TerminalCore::getScrollbackCell(int lineIndex, int col, ScrollbackCell& out) const
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

// ═══════════════════════════════════════════════════════════════════
//  光标
// ═══════════════════════════════════════════════════════════════════

VTermPos TerminalCore::cursorPosition() const
{
    VTermPos pos = {0, 0};
    if (_state)
        vterm_state_get_cursorpos(_state, &pos);
    return pos;
}

// ═══════════════════════════════════════════════════════════════════
//  libvterm 输出回调（C 风格 → Qt 信号）
// ═══════════════════════════════════════════════════════════════════

void TerminalCore::onOutput(const char* s, size_t len, void* user)
{
    auto* self = static_cast<TerminalCore*>(user);
    emit self->outputData(QByteArray(s, static_cast<int>(len)));
}

// ═══════════════════════════════════════════════════════════════════
//  VTermScreen 回调实现
// ═══════════════════════════════════════════════════════════════════

int TerminalCore::onDamage(VTermRect rect, void* user)
{
    auto* self = static_cast<TerminalCore*>(user);
    emit self->damage(rect);
    return 1;
}

int TerminalCore::onMoverect(VTermRect dest, VTermRect src, void* user)
{
    Q_UNUSED(dest);
    Q_UNUSED(src);
    // libvterm 在 moverect 后会自动发两个 damage rect（src 区域 + dest 区域），
    // 我们不需要额外处理。
    return 1;
}

int TerminalCore::onMovecursor(VTermPos pos, VTermPos oldpos, int visible, void* user)
{
    auto* self = static_cast<TerminalCore*>(user);
    self->_cursorVisible = (visible != 0);
    emit self->cursorMoved();
    return 1;
}

int TerminalCore::onSetTermProp(VTermProp prop, VTermValue* val, void* user)
{
    auto* self = static_cast<TerminalCore*>(user);
    switch (prop) {
    case VTERM_PROP_TITLE:
        if (val->string.str) {
            self->_title = QString::fromUtf8(val->string.str, static_cast<int>(val->string.len));
            emit self->titleChanged(self->_title);
        }
        break;
    case VTERM_PROP_CURSORVISIBLE:
        self->_cursorVisible = (val->boolean != 0);
        break;
    case VTERM_PROP_CURSORBLINK:
        self->_cursorBlink = (val->boolean != 0);
        break;
    case VTERM_PROP_CURSORSHAPE:
        self->_cursorShape = val->number;
        break;
    default:
        break;
    }
    return 1;
}

int TerminalCore::onBell(void* user)
{
    auto* self = static_cast<TerminalCore*>(user);
    emit self->bell();
    return 1;
}

int TerminalCore::onResize(int rows, int cols, void* user)
{
    auto* self = static_cast<TerminalCore*>(user);
    self->_cols = cols;
    self->_rows = rows;
    return 1;
}

int TerminalCore::onSbPushLine(int cols, const VTermScreenCell* cells, void* user)
{
    auto* self = static_cast<TerminalCore*>(user);
    self->_scrollback.pushLine(cells, cols);
    emit self->scrollbackChanged();
    return 1;
}

int TerminalCore::onSbPopLine(int cols, VTermScreenCell* cells, void* user)
{
    auto* self = static_cast<TerminalCore*>(user);
    return self->_scrollback.popLine(cells, cols) ? 1 : 0;
}

int TerminalCore::onSbClear(void* user)
{
    auto* self = static_cast<TerminalCore*>(user);
    self->_scrollback.clear();
    return 1;
}
