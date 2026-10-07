/**
 * @file   VTAdapter.cpp
 * @brief  libvterm 解析适配器实现。
 *
 * 详见 VTAdapter.h 的接口说明。本文件实现：
 * - VTermColor ↔ TerminalColor / VTermScreenCell ↔ Cell 的双向转换
 * - libvterm 的 C 回调（onDamage / onMoveRect / onScrollbackPush 等）
 *   桥接到 Observer 的 std::function
 */
#include "VTAdapter.h"

#include "core/CoreTypes.h"

#include <vterm.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace NovaTerm {
namespace {

// OSC 52 解码缓冲上限：512 KiB 文本（约 50 万字符），超出部分被 libvterm
// 截断。缓冲与 Impl 同寿命、复用不重复分配。
constexpr size_t kSelectionBufferBytes = 512 * 1024;

// tmux DCS passthrough 的魔法前缀与扫描上界。
constexpr char kTmuxMagic[] = "\x1bPtmux;";
constexpr size_t kTmuxMagicLen = sizeof(kTmuxMagic) - 1;
// 载荷上界：容纳最大 OSC 52（512 KiB 解码 ≈ 683 KiB base64）再留余量，
// 防止损坏流把扫描器钉死在 Payload 态吞掉后续所有输入。
constexpr size_t kMaxPassthroughBodyBytes = 2 * 1024 * 1024;
// 递归解包裹深度上界（tmux 套 tmux），超出按普通输入直接喂。
constexpr int kMaxPassthroughDepth = 8;

TerminalColor fromVTermColor(const VTermColor& source)
{
    TerminalColor color;
    // libvterm 的默认色同时携带 indexed/RGB 表示。先保留默认标志，
    // 让展示层区分"继承自终端方案"与"远端显式指定"的颜色。
    if (VTERM_COLOR_IS_DEFAULT_FG(&source)
        || VTERM_COLOR_IS_DEFAULT_BG(&source)) {
        color.type = ColorType::Default;
    } else if (VTERM_COLOR_IS_INDEXED(&source)) {
        color.type = ColorType::Indexed;
        color.index = source.indexed.idx;
    } else if (VTERM_COLOR_IS_RGB(&source)) {
        color.type = ColorType::Rgb;
        color.red = source.rgb.red;
        color.green = source.rgb.green;
        color.blue = source.rgb.blue;
    }
    return color;
}

VTermColor toVTermColor(const TerminalColor& source, bool foreground)
{
    VTermColor color{};
    switch (source.type) {
    case ColorType::Indexed:
        color.type = VTERM_COLOR_INDEXED;
        color.indexed.idx = source.index;
        break;
    case ColorType::Rgb:
        color.type = VTERM_COLOR_RGB;
        color.rgb.red = source.red;
        color.rgb.green = source.green;
        color.rgb.blue = source.blue;
        break;
    case ColorType::Default:
        color.type = foreground ? VTERM_COLOR_DEFAULT_FG : VTERM_COLOR_DEFAULT_BG;
        break;
    }
    return color;
}

CellAttributes fromVTermAttributes(const VTermScreenCellAttrs& source)
{
    CellAttributes attributes;
    attributes.bold = source.bold;
    attributes.underline = source.underline != VTERM_UNDERLINE_OFF;
    attributes.italic = source.italic;
    attributes.blink = source.blink;
    attributes.reverse = source.reverse;
    attributes.conceal = source.conceal;
    attributes.strike = source.strike;
    attributes.font = source.font != 0;
    attributes.dwl = source.dwl;
    attributes.dhl = source.dhl != 0;
    attributes.smallFont = source.small_font;
    attributes.baseline = source.baseline != VTERM_BASELINE_NORMAL;
    attributes.underlineStyle =
        source.underline == VTERM_UNDERLINE_DOUBLE ? UnderlineStyle::Double
        : source.underline == VTERM_UNDERLINE_CURLY ? UnderlineStyle::Curly
        : source.underline == VTERM_UNDERLINE_SINGLE ? UnderlineStyle::Single
                                                    : UnderlineStyle::Off;
    return attributes;
}

VTermScreenCellAttrs toVTermAttributes(const CellAttributes& source)
{
    VTermScreenCellAttrs attributes{};
    attributes.bold = source.bold;
    attributes.underline =
        source.underlineStyle == UnderlineStyle::Double ? VTERM_UNDERLINE_DOUBLE
        : source.underlineStyle == UnderlineStyle::Curly ? VTERM_UNDERLINE_CURLY
        : source.underline ? VTERM_UNDERLINE_SINGLE : VTERM_UNDERLINE_OFF;
    attributes.italic = source.italic;
    attributes.blink = source.blink;
    attributes.reverse = source.reverse;
    attributes.conceal = source.conceal;
    attributes.strike = source.strike;
    attributes.font = source.font ? 1 : 0;
    attributes.dwl = source.dwl;
    attributes.dhl = source.dhl ? 1 : 0;
    attributes.small_font = source.smallFont;
    attributes.baseline = source.baseline ? VTERM_BASELINE_RAISE
                                          : VTERM_BASELINE_NORMAL;
    return attributes;
}

void populateCell(const VTermScreenCell& source, Cell& cell)
{
    const int count = std::min(MaxCharsPerCell, VTERM_MAX_CHARS_PER_CELL);
    // VTerm 以 0 终止字符序列。多数 Cell 只有 0 或 1 个码点，若直接拷贝
    // 全部 6 槽会放大每行 scrollback 的成本，并可能拷贝到回调缓冲区中
    // 越过终止符的陈旧数据。
    int index = 0;
    for (; index < count && source.chars[index] != 0; ++index)
        cell.chars[index] = source.chars[index];
    if (index < count)
        cell.chars[index] = 0;
    cell.width = static_cast<uint8_t>(std::max(1, int(source.width)));
    cell.attributes = fromVTermAttributes(source.attrs);
    cell.foreground = fromVTermColor(source.fg);
    cell.background = fromVTermColor(source.bg);
}

Cell fromVTermCell(const VTermScreenCell& source)
{
    Cell cell;
    populateCell(source, cell);
    return cell;
}

VTermScreenCell toVTermCell(const Cell& source)
{
    VTermScreenCell cell{};
    const int count = std::min(MaxCharsPerCell, VTERM_MAX_CHARS_PER_CELL);
    std::copy_n(source.chars.begin(), count, cell.chars);
    cell.width = static_cast<char>(source.width);
    cell.attrs = toVTermAttributes(source.attributes);
    cell.fg = toVTermColor(source.foreground, true);
    cell.bg = toVTermColor(source.background, false);
    return cell;
}

bool isDefaultBlankCell(const VTermScreenCell& cell)
{
    const VTermScreenCellAttrs& attributes = cell.attrs;
    return cell.chars[0] == 0
        && cell.width <= 1
        && !attributes.bold
        && attributes.underline == VTERM_UNDERLINE_OFF
        && !attributes.italic
        && !attributes.blink
        && !attributes.reverse
        && !attributes.conceal
        && !attributes.strike
        && !attributes.font
        && !attributes.dwl
        && !attributes.dhl
        && !attributes.small_font
        && attributes.baseline == VTERM_BASELINE_NORMAL
        && VTERM_COLOR_IS_DEFAULT_FG(&cell.fg)
        && VTERM_COLOR_IS_DEFAULT_BG(&cell.bg);
}

CursorShape fromVTermCursorShape(int shape)
{
    if (shape == VTERM_PROP_CURSORSHAPE_UNDERLINE)
        return CursorShape::Underline;
    if (shape == VTERM_PROP_CURSORSHAPE_BAR_LEFT)
        return CursorShape::BarLeft;
    return CursorShape::Block;
}

} // namespace

class VTAdapter::Impl
{
public:
    Impl(int columns, int rows, ScreenBuffer& screen,
         ScrollbackBuffer& scrollback, Observer observer)
        : screen(screen)
        , scrollback(scrollback)
        , observer(std::move(observer))
    {
        vt = vterm_new(rows, columns);
        if (!vt) {
            std::fprintf(stderr, "VTAdapter: vterm_new() failed\n");
            return;
        }

        vts = vterm_obtain_screen(vt);
        state = vterm_obtain_state(vt);
        vterm_state_reset(state, 0);
        vterm_set_utf8(vt, 1);
        vterm_screen_enable_altscreen(vts, 1);
        // 视口变窄时保留已有终端内容。libvterm 原生 reflow 不可用时，
        // resize 会截断每行已填充内容的右侧，直到新输出到达。
        vterm_screen_enable_reflow(vts, true);
        vterm_screen_set_damage_merge(vts, VTERM_DAMAGE_SCROLL);
        vterm_output_set_callback(vt, &Impl::onOutput, this);

        std::memset(&callbacks, 0, sizeof(callbacks));
        callbacks.damage = &Impl::onDamage;
        callbacks.moverect = &Impl::onMoveRect;
        callbacks.movecursor = &Impl::onMoveCursor;
        callbacks.settermprop = &Impl::onSetTermProperty;
        callbacks.bell = &Impl::onBell;
        callbacks.resize = &Impl::onResize;
        callbacks.sb_pushline_ex = &Impl::onScrollbackPush;
        callbacks.sb_popline_ex = &Impl::onScrollbackPop;
        callbacks.sb_clear = &Impl::onScrollbackClear;
        vterm_screen_set_callbacks(vts, &callbacks, this);

        // OSC 52（剪贴板转义序列）：vim/nvim 的 osc52 provider、
        // opencode、claude code 等经此把选中文本写入系统剪贴板或查询
        // 剪贴板。libvterm 只做 base64 编解码与分片，落地到系统剪贴板
        // 由 observer 完成。回调结构体与解码缓冲都必须与 Impl 同寿命（libvterm 只存指针）。缓冲自持避免依赖 libvterm
        // 的分配器回收；超出上限的载荷被截断。
        std::memset(&selectionCallbacks, 0, sizeof(selectionCallbacks));
        selectionCallbacks.set = &Impl::onSelectionSet;
        selectionCallbacks.query = &Impl::onSelectionQuery;
        selectionBuffer.resize(kSelectionBufferBytes);
        vterm_state_set_selection_callbacks(state, &selectionCallbacks, this,
                                            selectionBuffer.data(),
                                            selectionBuffer.size());
    }

    ~Impl()
    {
        if (vt)
            vterm_free(vt);
    }

    // 从 libvterm 同步指定矩形区域到本地 ScreenBuffer。
    // 按行批量读取：每行一次跨库调用 + 一次行基址计算，取代逐格
    // vterm_screen_get_cell + 逐格 ScreenBuffer::setCell 的每格开销。
    void syncRegion(VTermRect rectangle)
    {
        if (!vts)
            return;

        const int startRow = std::clamp(rectangle.start_row, 0, screen.rows());
        const int endRow = std::clamp(rectangle.end_row, 0, screen.rows());
        const int startColumn =
            std::clamp(rectangle.start_col, 0, screen.columns());
        const int endColumn =
            std::clamp(rectangle.end_col, 0, screen.columns());
        const int cellCount = endColumn - startColumn;
        if (cellCount <= 0)
            return;

        // 行缓冲跨行/跨回调复用，避免每次同步都分配。
        _rowCells.resize(std::size_t(cellCount));
        for (int row = startRow; row < endRow; ++row) {
            const int count = vterm_screen_get_cells(
                vts, row, startColumn, endColumn, _rowCells.data());
            if (count <= 0)
                continue;
            Cell* destination = screen.writableRowSpan(row, startColumn, count);
            if (!destination)
                continue;
            for (int index = 0; index < count; ++index)
                destination[index] = fromVTermCell(_rowCells[std::size_t(index)]);
        }
    }

    /**
     * @brief 从 libvterm 同步每行的软换行（continuation）标志。
     * @note  libvterm 是该状态的唯一真源。moveRect 收到的是任意矩形，无法
     *        从 Cell 拷贝推断行语义，因此统一在全量同步边界重读整屏
     *        lineinfo。行数量级约 100，开销可忽略。
     */
    void syncLineInfo()
    {
        if (!state)
            return;
        const int rows = screen.rows();
        for (int row = 0; row < rows; ++row) {
            const VTermLineInfo* info = vterm_state_get_lineinfo(state, row);
            screen.setRowContinuation(row, info && info->continuation != 0);
        }
    }

    // ── libvterm C 回调（静态函数指针，user 指针为 Impl 实例）──

    // libvterm 输出字节流（如查询回复、终端响应）。
    static void onOutput(const char* data, size_t length, void* user)
    {
        auto& self = *static_cast<Impl*>(user);
        if (self.observer.output)
            self.observer.output(ByteView(data, static_cast<isize>(length)));
    }

    // 屏幕区域被修改：同步本地缓冲并通知 observer。
    static int onDamage(VTermRect rectangle, void* user)
    {
        auto& self = *static_cast<Impl*>(user);
        self.syncRegion(rectangle);
        if (self.observer.damage) {
            self.observer.damage({rectangle.start_row, rectangle.end_row,
                                  rectangle.start_col, rectangle.end_col});
        }
        return 1;
    }

    // 区域拷贝（光标滚动、区域滚动）。
    static int onMoveRect(VTermRect destination, VTermRect, void* user)
    {
        auto& self = *static_cast<Impl*>(user);
        const DirtyRegion destinationRegion{
            destination.start_row, destination.end_row,
            destination.start_col, destination.end_col};
        // SCROLL 合并模式延迟发布 moverect；本地源区域可能尚未同步，
        // 且 libvterm 已执行后续擦除或改写。必须读取当前目标区域，
        // 不能再次搬移本地旧 Cell，否则结果会依赖输入分批边界。
        self.syncRegion(destination);
        if (self.observer.damage)
            self.observer.damage(destinationRegion);
        return 1;
    }

    // 光标移动。
    static int onMoveCursor(VTermPos position, VTermPos, int visible, void* user)
    {
        auto& self = *static_cast<Impl*>(user);
        self.cursorState.position = {position.row, position.col};
        self.cursorState.visible = visible != 0;
        if (self.observer.cursorChanged)
            self.observer.cursorChanged(self.cursorState);
        return 1;
    }

    // 终端属性变化：标题、光标可见性/闪烁/形状。
    static int onSetTermProperty(VTermProp property, VTermValue* value, void* user)
    {
        auto& self = *static_cast<Impl*>(user);
        bool cursorChanged = false;
        switch (property) {
        case VTERM_PROP_ALTSCREEN: {
            const bool active = value->boolean != 0;
            if (self.alternateScreen != active) {
                self.alternateScreen = active;
                if (self.observer.alternateScreenChanged)
                    self.observer.alternateScreenChanged(active);
            }
            break;
        }
        case VTERM_PROP_MOUSE: {
            // libvterm 上报的是 VTermMouseProp（NONE/CLICK/DRAG/MOVE，
            // 值 0..3），与 MouseTrackingMode 的枚举值一一对应。
            const auto mode = MouseTrackingMode(
                std::clamp(value->number, 0, int(MouseTrackingMode::Move)));
            if (self.mouseMode != mode) {
                self.mouseMode = mode;
                if (self.observer.mouseModeChanged)
                    self.observer.mouseModeChanged(mode);
            }
            break;
        }
        case VTERM_PROP_TITLE:
            if (value->string.str) {
                // libvterm 的标题已是 UTF-8，直接按字节构造 std::string。
                self.title.assign(value->string.str, value->string.len);
                if (self.observer.titleChanged)
                    self.observer.titleChanged(self.title);
            }
            break;
        case VTERM_PROP_CURSORVISIBLE:
            cursorChanged = self.cursorState.visible != (value->boolean != 0);
            self.cursorState.visible = value->boolean != 0;
            break;
        case VTERM_PROP_CURSORBLINK:
            cursorChanged = self.cursorState.blink != (value->boolean != 0);
            self.cursorState.blink = value->boolean != 0;
            break;
        case VTERM_PROP_CURSORSHAPE: {
            const CursorShape shape = fromVTermCursorShape(value->number);
            cursorChanged = self.cursorState.shape != shape;
            self.cursorState.shape = shape;
            break;
        }
        default:
            break;
        }
        if (cursorChanged && self.observer.cursorChanged)
            self.observer.cursorChanged(self.cursorState);
        return 1;
    }

    static int onBell(void* user)
    {
        auto& self = *static_cast<Impl*>(user);
        if (self.observer.bell)
            self.observer.bell();
        return 1;
    }

    // OSC 52 写剪贴板：libvterm 已完成 base64 解码，分片经
    // pendingSelection 累积，final 时一次性交给 observer。
    // VTERM_SELECTION_* 与 NovaTerm::Selection 的位值一一对应，直接转换。
    static int onSelectionSet(VTermSelectionMask mask,
                              VTermStringFragment frag, void* user)
    {
        auto& self = *static_cast<Impl*>(user);
        if (frag.initial) {
            self.pendingSelection.clear();
            self.pendingSelectionMask = int(mask);
        }
        self.pendingSelection.append(frag.str, frag.len);
        if (frag.final && self.observer.selectionSet) {
            self.observer.selectionSet(self.pendingSelectionMask,
                                       self.pendingSelection);
        }
        return 1;
    }

    // OSC 52 查询剪贴板：应答经 observer 走 GUI 线程读剪贴板，
    // 再由 sendSelection() 编码发回。
    static int onSelectionQuery(VTermSelectionMask mask, void* user)
    {
        auto& self = *static_cast<Impl*>(user);
        if (self.observer.selectionQuery)
            self.observer.selectionQuery(int(mask));
        return 1;
    }

    // ── tmux DCS passthrough 预扫描 ─────────────────────────────
    // 应用在 tmux 内检测到 $TMUX 时，会把 OSC 52 等序列自包裹成
    // `ESC P tmux ; <ESC 加倍的载荷> ESC \`（vim/nvim 的 osc52 provider、
    // opencode、claude code、sshclip 都这么做），期望外层终端解开内层
    // 序列按普通输入处理。libvterm 的 parser 在字符串态遇到 ESC ESC 后
    // 跟随非 '\' 字节会直接中止 DCS 并把后续字节当正文打印（parser.c 的
    // abort 分支），因此必须在喂给 libvterm 之前自行解开。
    // 扫描状态跨 writeInput 调用保持，支持序列任意分片到达。
    void feedWithPassthrough(ByteView data, int depth)
    {
        // 普通路径按连续段一次性喂给 libvterm，避免逐字节跨库调用。
        //
        // runStart 的不变式：它等于 data 中「尚未被状态机消费」的第一个字节
        // 下标 —— 既没写给 libvterm，也没扣在 scanProbe / passthroughBody 里。
        // 状态机消费掉第 i 字节（写给 libvterm 或扣进探针/载荷）后必须推进到
        // i+1；Normal 态的非 ESC 字节不被消费，runStart 原地不动。
        //
        // 该不变式是跨 writeInput 分片正确的前提：分片末尾的
        // flushRun(data.size()) 只写 [runStart, end)，扣在探针/载荷里的字节都
        // 在该区间之前，不会被重复喂给 libvterm。反之漏推进则会让分片结尾把
        // 已扣住的字节（或已写过的前缀）再喂一遍 —— 表现为重复字节，或让
        // libvterm 停在 DCS 字符串态永不终止。
        size_t runStart = 0;
        const auto flushRun = [&](size_t end) {
            if (end > runStart)
                vterm_input_write(vt, data.data + runStart, end - runStart);
        };
        // 第 i 字节已被消费（写出或扣住）→ 推进扣字节游标。
        const auto consumed = [&](isize i) { runStart = size_t(i) + 1; };
        for (isize i = 0; i < data.size; ++i) {
            const char byte = data.data[i];
            switch (passthroughScan) {
            case PassthroughScan::Normal:
                if (byte == '\x1b') {
                    flushRun(size_t(i));
                    passthroughScan = PassthroughScan::Esc;
                    // 该 ESC 转入探测态等待后继字节，扣住不发。
                    consumed(i);
                }
                break;
            case PassthroughScan::Esc:
                if (byte == 'P') {
                    scanProbe.assign("\x1bP", 2);
                    passthroughScan = PassthroughScan::Probe;
                    consumed(i);
                } else {
                    // 不是 DCS：ESC 与当前字节按原顺序送回解析器。
                    // 连续 ESC 保持探测态（下一字节才决定去向）。
                    vterm_input_write(vt, "\x1b", 1);
                    if (byte != '\x1b') {
                        vterm_input_write(vt, &byte, 1);
                        passthroughScan = PassthroughScan::Normal;
                    }
                    // byte == '\x1b' 时它是新的待定 ESC，同样已消费。
                    consumed(i);
                }
                break;
            case PassthroughScan::Probe:
                scanProbe.push_back(byte);
                consumed(i);
                if (size_t(scanProbe.size()) <= kTmuxMagicLen
                    && std::memcmp(scanProbe.data(), kTmuxMagic,
                                   size_t(scanProbe.size())) == 0) {
                    if (size_t(scanProbe.size()) == kTmuxMagicLen) {
                        passthroughBody.clear();
                        passthroughScan = PassthroughScan::Payload;
                    }
                    break;
                }
                // 前缀不匹配：已积累字节（不含当前字节）原样送回解析器，
                // 当前字节按 Normal 重新分派 —— 非 tmux 的 DCS 行为不变。
                // scanProbe 首字节就是那枚待定 ESC，故写入 size()-1 字节
                // 恰为「ESC + 已匹配前缀」，当前字节另行分派。
                vterm_input_write(vt, scanProbe.data(),
                                  size_t(scanProbe.size()) - 1);
                scanProbe.clear();
                passthroughScan = PassthroughScan::Normal;
                if (byte == '\x1b') {
                    passthroughScan = PassthroughScan::Esc;
                } else {
                    vterm_input_write(vt, &byte, 1);
                }
                consumed(i);
                break;
            case PassthroughScan::Payload:
                passthroughBody.push_back(byte);
                consumed(i);
                if (passthroughBody.size() > kMaxPassthroughBodyBytes) {
                    // 恶意/损坏流的上界保护：丢弃积累并回到普通扫描。
                    passthroughBody.clear();
                    passthroughScan = PassthroughScan::Normal;
                } else if (byte == '\x1b') {
                    passthroughScan = PassthroughScan::PayloadEsc;
                }
                break;
            case PassthroughScan::PayloadEsc:
                if (byte == '\\') {
                    // ST 终止：body 末字节是终止符的 ESC，剥掉后解开回喂。
                    passthroughBody.pop_back();
                    std::string inner = unescapeTmuxBody();
                    passthroughBody.clear();
                    passthroughScan = PassthroughScan::Normal;
                    consumed(i);
                    if (!inner.empty()) {
                        if (depth < kMaxPassthroughDepth) {
                            // 内层可能再次嵌套 passthrough（tmux 套 tmux），
                            // 递归过扫描；超深则按普通输入直接喂。
                            feedWithPassthrough(
                                ByteView(inner.data(),
                                         isize(inner.size())),
                                depth + 1);
                        } else {
                            vterm_input_write(vt, inner.data(), inner.size());
                        }
                    }
                } else {
                    // 加倍的 ESC（ESC ESC）或载荷普通字节。
                    passthroughBody.push_back(byte);
                    consumed(i);
                    if (passthroughBody.size() > kMaxPassthroughBodyBytes) {
                        passthroughBody.clear();
                        passthroughScan = PassthroughScan::Normal;
                    } else {
                        passthroughScan = PassthroughScan::Payload;
                    }
                }
                break;
            }
        }
        flushRun(size_t(data.size));
    }

    // 解开 tmux 载荷的 ESC 加倍（ESC ESC → ESC）。
    std::string unescapeTmuxBody() const
    {
        std::string inner;
        inner.reserve(passthroughBody.size());
        for (size_t i = 0; i < passthroughBody.size(); ++i) {
            const char byte = passthroughBody[i];
            inner.push_back(byte);
            if (byte == '\x1b' && i + 1 < passthroughBody.size()
                && passthroughBody[i + 1] == '\x1b') {
                ++i;
            }
        }
        return inner;
    }

    // libvterm 内部 resize 回调：同步 ScreenBuffer 尺寸并全屏同步。
    static int onResize(int rows, int columns, void* user)
    {
        auto& self = *static_cast<Impl*>(user);
        self.screen.resize(columns, rows);
        self.syncRegion({0, rows, 0, columns});
        return 1;
    }

    // 活动屏幕行被推出到 scrollback：硬换行行去除尾部空格后转存。
    static int onScrollbackPush(int columns, const VTermScreenCell* cells,
                                int softWrapped, void* user)
    {
        auto& self = *static_cast<Impl*>(user);
        int storedColumns = columns;
        // 仅硬换行行可裁剪行尾默认空 Cell。软换行行的尾部空格是有效内容
        // —— 下一行的文本紧接其后，裁掉会让按新列宽重排后的内容整体左移。
        if (softWrapped == 0) {
            while (storedColumns > 0
                   && isDefaultBlankCell(cells[storedColumns - 1])) {
                --storedColumns;
            }
        }

        std::vector<Cell>& converted =
            self.scrollback.beginPushLine(columns, storedColumns);
        for (int column = 0; column < storedColumns; ++column)
            populateCell(cells[column], converted[column]);
        self.scrollback.commitPushLine(self.nextScrollbackContinuation,
                                       softWrapped == 0);
        self.nextScrollbackContinuation = softWrapped != 0;
        // 显式 resize 期间的行进入 scrollback 不是增量滚动，不视为活动屏幕
        // 上滚；resize 完成后会发布一次全屏 damage。
        if (!self.resizeInProgress && self.observer.screenScrolled)
            self.observer.screenScrolled(1);
        if (self.observer.scrollbackChanged)
            self.observer.scrollbackChanged();
        return 1;
    }

    // 从 scrollback 弹出一行（reverse index 越过顶部时触发）。
    static int onScrollbackPop(int columns, VTermScreenCell* cells,
                              int* continuation, void* user)
    {
        auto& self = *static_cast<Impl*>(user);
        std::vector<Cell> converted(columns);
        bool hasPrefix = false;
        if (!self.scrollback.popLine(converted.data(), columns, &hasPrefix))
            return 0;
        *continuation = hasPrefix ? 1 : 0;
        // 取回的尾段现在属于屏幕，后续推出行应以屏幕边界为准。
        self.nextScrollbackContinuation = hasPrefix;
        for (int column = 0; column < columns; ++column)
            cells[column] = toVTermCell(converted[column]);
        if (self.observer.scrollbackChanged)
            self.observer.scrollbackChanged();
        return 1;
    }

    static int onScrollbackClear(void* user)
    {
        auto& self = *static_cast<Impl*>(user);
        self.scrollback.clear();
        self.nextScrollbackContinuation = false;
        if (self.observer.scrollbackChanged)
            self.observer.scrollbackChanged();
        return 1;
    }

    ScreenBuffer& screen;
    ScrollbackBuffer& scrollback;
    Observer observer;
    CursorState cursorState;
    std::string title;
    bool alternateScreen{false};
    MouseTrackingMode mouseMode{MouseTrackingMode::None};
    // OSC 52 选区回调与解码缓冲：与 Impl 同寿命，libvterm 只存指针。
    VTermSelectionCallbacks selectionCallbacks{};
    std::vector<char> selectionBuffer;
    std::string pendingSelection;    // 跨分片累积的解码文本
    int pendingSelectionMask{0};     // 分片开始时的 Selection 位组合
    // tmux DCS passthrough 预扫描状态（见 feedWithPassthrough）。
    enum class PassthroughScan
    {
        Normal,      // 普通字节，直接喂 libvterm
        Esc,         // 见到 ESC，等待判断是否 DCS
        Probe,       // 候选 "ESC P tmux;" 前缀
        Payload,     // passthrough 载荷积累中
        PayloadEsc   // 载荷中见到 ESC（加倍或终止符）
    };
    PassthroughScan passthroughScan{PassthroughScan::Normal};
    std::string scanProbe;           // Probe 态的候选前缀（≤ kTmuxMagicLen）
    std::string passthroughBody;     // Payload 态的原始载荷（含 ESC 加倍）
    VTerm* vt{nullptr};
    VTermScreen* vts{nullptr};
    VTermState* state{nullptr};
    VTermScreenCallbacks callbacks{};
    // syncRegion 的按行读取缓冲：跨行、跨回调复用，避免每次同步都分配。
    std::vector<VTermScreenCell> _rowCells;
    bool nextScrollbackContinuation{false};  // 下一行是否为前一行的软换行延续
    bool resizeInProgress{false};             // resize 进行中标记，抑制 screenScrolled 信号
};

VTAdapter::VTAdapter(int columns, int rows, ScreenBuffer& screen,
                     ScrollbackBuffer& scrollback, Observer observer)
    : _impl(std::make_unique<Impl>(columns, rows, screen, scrollback,
                                  std::move(observer)))
{
}

VTAdapter::~VTAdapter() = default;

bool VTAdapter::isValid() const
{
    return _impl && _impl->vt;
}

void VTAdapter::writeInput(ByteView data)
{
    if (!isValid())
        return;
    // 先过 tmux passthrough 预扫描：解开 `ESC P tmux; ... ESC \` 包裹的
    // 内层序列后按普通输入喂给 libvterm，其余字节原样透传。
    _impl->feedWithPassthrough(data, 0);
    // 解析可能改变任意行的软换行状态（自动换行、滚动、清屏），统一重读。
    _impl->syncLineInfo();
}

void VTAdapter::flushDamage()
{
    if (!isValid())
        return;
    vterm_screen_flush_damage(_impl->vts);
    _impl->syncLineInfo();
}

void VTAdapter::clearScreen()
{
    if (!isValid())
        return;
    // 用户清屏同时取消未完成的控制序列，避免清屏指令被当成 OSC/DCS
    // 内容。直接在适配器内解析，不向远端发送字节，也不重置终端模式。
    _impl->passthroughScan = Impl::PassthroughScan::Normal;
    _impl->scanProbe.clear();
    _impl->passthroughBody.clear();
    constexpr char sequence[] = "\x18\x1b[2J\x1b[H";
    vterm_input_write(_impl->vt, sequence, sizeof(sequence) - 1);
    flushDamage();
}

void VTAdapter::resize(int columns, int rows)
{
    if (isValid()) {
        // ScopedValueRollback 保证 resize 完成后自动复位 resizeInProgress。
        const NovaTerm::ScopedValueRollback<bool> resizeGuard(
            _impl->resizeInProgress, true);
        vterm_set_size(_impl->vt, rows, columns);
        // libvterm 在 screen resize 回调返回后才回写 VTermState 的 lineinfos
        // 与活动 lineinfo 指针；回调内读取会访问刚释放的旧数组。
        _impl->syncLineInfo();
        // libvterm 的 resize 回调会同步 ScreenBuffer，但 resize 不一定产生
        // 常规解析 damage（无字节到达时）。显式发布全区域，确保异步模型
        // resize 总是失效所有缓存的 CPU/GPU 行。
        if (_impl->observer.damage)
            _impl->observer.damage({0, rows, 0, columns});
    }
}

void VTAdapter::setLfImpliesCr(bool enabled)
{
    if (isValid())
        vterm_state_set_lf_implies_cr(_impl->state, enabled);
}

void VTAdapter::setDefaultColors(const TerminalColor& foreground,
                                 const TerminalColor& background)
{
    if (!isValid())
        return;
    VTermColor vtForeground = toVTermColor(foreground, true);
    VTermColor vtBackground = toVTermColor(background, false);
    vterm_screen_set_default_colors(_impl->vts, &vtForeground, &vtBackground);
}

void VTAdapter::keyboardUnichar(uint32_t codepoint, int modifiers)
{
    if (isValid()) {
        vterm_keyboard_unichar(
            _impl->vt, codepoint, static_cast<VTermModifier>(modifiers));
    }
}

void VTAdapter::keyboardKey(int key, int modifiers)
{
    if (isValid()) {
        vterm_keyboard_key(_impl->vt, static_cast<VTermKey>(key),
                           static_cast<VTermModifier>(modifiers));
    }
}

void VTAdapter::startPaste()
{
    if (isValid())
        vterm_keyboard_start_paste(_impl->vt);
}

void VTAdapter::endPaste()
{
    if (isValid())
        vterm_keyboard_end_paste(_impl->vt);
}

void VTAdapter::mouseButton(int button, bool pressed, int modifiers)
{
    if (isValid()) {
        vterm_mouse_button(_impl->vt, button, pressed,
                           static_cast<VTermModifier>(modifiers));
    }
}

void VTAdapter::mouseMove(int row, int col, int modifiers)
{
    if (isValid()) {
        vterm_mouse_move(_impl->vt, row, col,
                         static_cast<VTermModifier>(modifiers));
    }
}

void VTAdapter::focusIn()
{
    if (isValid())
        vterm_state_focus_in(_impl->state);
}

void VTAdapter::focusOut()
{
    if (isValid())
        vterm_state_focus_out(_impl->state);
}

CursorState VTAdapter::cursor() const
{
    return _impl ? _impl->cursorState : CursorState{};
}

std::string VTAdapter::title() const
{
    return _impl ? _impl->title : std::string{};
}

bool VTAdapter::alternateScreen() const
{
    return _impl && _impl->alternateScreen;
}

MouseTrackingMode VTAdapter::mouseMode() const
{
    return _impl ? _impl->mouseMode : MouseTrackingMode::None;
}

void VTAdapter::sendSelection(int mask, const std::string& utf8)
{
    if (!isValid() || utf8.empty())
        return;
    // vterm_state_send_selection 自行做 base64 编码并推 OSC 52；
    // Selection 位值与 VTermSelectionMask 一一对应。应答只保留
    // Clipboard 位，避免把多目标查询展开成多条应答。
    const int answerMask = mask & int(Selection::Clipboard);
    if (!answerMask)
        return;
    const VTermStringFragment fragment{utf8.data(), utf8.size(), true, true};
    vterm_state_send_selection(
        _impl->state, static_cast<VTermSelectionMask>(answerMask), fragment);
}

} // namespace NovaTerm
