/**
 * @file   VTAdapter.h
 * @brief  libvterm 解析适配器。
 *
 * 封装 libvterm 的会话句柄与回调，把 VT 字节流转换为 ScreenBuffer /
 * ScrollbackBuffer 的写入、CursorState 更新与外部信号。不可拷贝，
 * 由 TerminalCore::Runtime 在专用工作线程内独占持有。
 */
#pragma once

#include "ScreenBuffer.h"
#include "ScrollbackBuffer.h"
#include "core/CoreTypes.h"

#include <functional>
#include <memory>
#include <string>

namespace NovaTerm {

// libvterm 解析适配器。封装 vterm_new / vterm_input_write / vterm_screen_set_callbacks
// 等调用，把 libvterm 的 C 回调桥接为 Observer 中的 std::function。
class VTAdapter
{
public:
    // 回调集合：调用方实现这些函数以接收解析器输出。
    struct Observer
    {
        std::function<void(ByteView)> output;                // 终端响应字节（如查询回复）
        std::function<void(const DirtyRegion&)> damage;      // 屏幕区域被修改
        std::function<void(const CursorState&)> cursorChanged; // 光标状态变化
        std::function<void(const std::string&)> titleChanged; // 终端标题更新（OSC 0/2），UTF-8
        std::function<void()> bell;                          // BEL 信号
        std::function<void()> scrollbackChanged;             // 滚动历史变更
        std::function<void(int)> screenScrolled;             // 活动屏幕上滚行数
        std::function<void(bool)> alternateScreenChanged;    // 备用屏进入/退出
        std::function<void(MouseTrackingMode)> mouseModeChanged; // 鼠标跟踪模式变更
        // OSC 52：应用请求写剪贴板（mask 为 Selection 位组合，text 为解码
        // 后的 UTF-8；空串表示清除）。worker 线程回调，门面须自行切到 GUI 线程。
        std::function<void(int mask, const std::string&)> selectionSet;
        // OSC 52 查询：应用想读剪贴板，应答走 sendSelection()。
        std::function<void(int mask)> selectionQuery;
    };

    /**
     * @brief 构造 VT 解析适配器。
     * @param columns 初始列数。
     * @param rows 初始行数。
     * @param screen 屏幕缓冲（外部拥有，本类不接管生命周期）。
     * @param scrollback 滚动历史缓冲（外部拥有）。
     * @param observer 回调集合。
     */
    VTAdapter(int columns, int rows, ScreenBuffer& screen,
              ScrollbackBuffer& scrollback, Observer observer);
    ~VTAdapter();

    VTAdapter(const VTAdapter&) = delete;
    VTAdapter& operator=(const VTAdapter&) = delete;
    // 移动同样显式删除：由 Runtime 在工作线程内原地独占持有，不存在
    // 转移所有权的场景；显式声明避免依赖「拷贝删除隐式抑制移动」。
    VTAdapter(VTAdapter&&) = delete;
    VTAdapter& operator=(VTAdapter&&) = delete;

    bool isValid() const;

    /**
     * @brief 写入字节流到 libvterm 解析器。
     *        在持有外部 modelMutex 的线程内调用，回调同步触发。
     */
    void writeInput(ByteView data);

    /**
     * @brief 刷新尚未发布的脏区域，触发 damage 回调。
     */
    void flushDamage();

    /**
     * @brief 通知终端尺寸变更（触发 TIOCSWINSZ 等价行为）。
     */
    void resize(int columns, int rows);

    /**
     * @brief 设置终端默认前景/背景色（用于未指定颜色的 Cell）。
     */
    void setDefaultColors(const TerminalColor& foreground,
                          const TerminalColor& background);

    // ── 输入事件（转发到 libvterm 的 keyboard / mouse 接口）──
    void keyboardUnichar(uint32_t codepoint, int modifiers);
    void keyboardKey(int key, int modifiers);
    void startPaste();  // 通知终端开始括号粘贴模式
    void endPaste();    // 通知终端结束括号粘贴模式
    void mouseButton(int button, bool pressed, int modifiers);
    // 更新鼠标位置（终端单元格坐标）。libvterm 的按键事件不携带坐标，
    // 位置只能通过本函数设置；DRAG/MOVE 跟踪模式下还会触发移动上报。
    void mouseMove(int row, int col, int modifiers);
    void focusIn();
    void focusOut();

    CursorState cursor() const;
    std::string title() const;  // UTF-8
    [[nodiscard]] bool alternateScreen() const;
    [[nodiscard]] MouseTrackingMode mouseMode() const;

    /**
     * @brief 应答 OSC 52 查询：把本机剪贴板文本编码为 OSC 52 发回应用。
     * @param mask Selection 位组合（应答只取 Clipboard 位）。
     * @param utf8 剪贴板文本，UTF-8。空串不发送（空应答在协议里是"清除"）。
     * @note  仅可由 Parser 工作线程调用（与其他 adapter 方法相同）。
     */
    void sendSelection(int mask, const std::string& utf8);

private:
    // PImpl 模式隔离 libvterm 头文件依赖。
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace NovaTerm
