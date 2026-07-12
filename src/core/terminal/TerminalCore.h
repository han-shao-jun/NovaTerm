#pragma once
#include <QObject>
#include <QByteArray>
#include <QString>
#include <vterm.h>
#include "ScrollbackBuffer.h"

class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

// libvterm 的 RAII 封装，提供终端仿真核心功能：
//   • VTerm 生命周期管理
//   • 输入：raw bytes → vterm_input_write()
//   • 输出：vterm_output 回调 → outputData 信号（键盘转义序列）
//   • 屏幕：通过 vterm_screen_get_cell() 按需读取
//   • 回滚：ScrollbackBuffer 管理 sb_pushline/sb_popline 行
class TerminalCore : public QObject
{
    Q_OBJECT
public:
    explicit TerminalCore(int cols, int rows, QObject* parent = nullptr);
    ~TerminalCore() override;

    // ── I/O ────────────────────────────────────────────────────
    // PTY/Transport 输出 → libvterm 解析器
    void writeInput(const QByteArray& data);

    // 键盘/鼠标事件 → libvterm 编码 → outputData 信号
    void processKeyPress(QKeyEvent* event);
    void processMousePress(QMouseEvent* event);
    void processMouseMove(QMouseEvent* event);
    void processMouseRelease(QMouseEvent* event);
    void processWheel(QWheelEvent* event);

    // 粘贴文本（如 Ctrl+Shift+V）
    void pasteText(const QString& text);

    // ── 尺寸 ───────────────────────────────────────────────────
    void resize(int cols, int rows);
    int columns() const { return _cols; }
    int rows() const    { return _rows; }

    // ── 屏幕访问 ───────────────────────────────────────────────
    // 从 libvterm 内部 screen buffer 读取指定位置的 cell。
    // 返回 false 表示坐标越界。
    bool getCell(int row, int col, VTermScreenCell& out) const;
    void flushDamage();

    // ── Scrollback ─────────────────────────────────────────────
    int scrollbackLineCount() const;
    bool getScrollbackCell(int lineIndex, int col, ScrollbackCell& out) const;
    void setScrollbackLimit(int lines);
    void clearScrollback();

    // ── 光标 ───────────────────────────────────────────────────
    VTermPos cursorPosition() const;
    bool cursorVisible() const       { return _cursorVisible; }
    int cursorShape() const          { return _cursorShape; }
    bool cursorBlink() const         { return _cursorBlink; }

    // ── 属性 ───────────────────────────────────────────────────
    QString title() const            { return _title; }

    // ── 底层句柄（渲染器需要）──────────────────────────────────
    VTerm* vterm() const             { return _vt; }
    VTermScreen* screen() const      { return _vts; }
    VTermState* state() const        { return _state; }

signals:
    void outputData(const QByteArray& data);   // 键盘 → 转义序列，送往 transport
    void titleChanged(const QString& title);
    void bell();
    void damage(const VTermRect& rect);
    void cursorMoved();
    void scrollbackChanged();

private:
    // ── libvterm 输出回调（C 风格） ────────────────────────────
    static void onOutput(const char* s, size_t len, void* user);

    // ── VTermScreen 回调 ──────────────────────────────────────
    static int onDamage(VTermRect rect, void* user);
    static int onMoverect(VTermRect dest, VTermRect src, void* user);
    static int onMovecursor(VTermPos pos, VTermPos oldpos, int visible, void* user);
    static int onSetTermProp(VTermProp prop, VTermValue* val, void* user);
    static int onBell(void* user);
    static int onResize(int rows, int cols, void* user);
    static int onSbPushLine(int cols, const VTermScreenCell* cells, void* user);
    static int onSbPopLine(int cols, VTermScreenCell* cells, void* user);
    static int onSbClear(void* user);

    // ── 内部辅助 ──────────────────────────────────────────────
    void updateTerminalSize();
    QByteArray encodeKeyText(const QString& text, VTermModifier mod);

    VTerm*       _vt{nullptr};
    VTermScreen* _vts{nullptr};
    VTermState*  _state{nullptr};

    int _cols{80};
    int _rows{24};

    // 终端属性
    QString _title;
    bool _cursorVisible{true};
    int  _cursorShape{VTERM_PROP_CURSORSHAPE_BLOCK};
    bool _cursorBlink{true};

    // libvterm 只存指针不拷贝，必须将回调结构体保存为成员变量
    // 避免栈上局部变量析构后成为悬垂指针
    VTermScreenCallbacks _screenCallbacks;

    // Scrollback
    ScrollbackBuffer _scrollback;
};
