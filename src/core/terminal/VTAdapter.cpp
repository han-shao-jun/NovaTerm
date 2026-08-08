#include "VTAdapter.h"

#include <QDebug>
#include <QScopedValueRollback>
#include <vterm.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace NovaTerm {
namespace {

TerminalColor fromVTermColor(const VTermColor& source)
{
    TerminalColor color;
    // libvterm's default colours also carry an indexed/RGB representation.
    // Preserve the default flag first so presentation layers can distinguish
    // inherited colours from colours explicitly selected by the remote side.
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
    // VTerm terminates the character sequence with zero. Most terminal cells
    // contain zero or one code point, so copying all six slots multiplied the
    // cost of every scrollback row and could also copy stale data past the
    // terminator from callback-owned buffers.
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
            qCritical() << "VTAdapter: vterm_new() failed";
            return;
        }

        vts = vterm_obtain_screen(vt);
        state = vterm_obtain_state(vt);
        vterm_state_reset(state, 0);
        vterm_set_utf8(vt, 1);
        vterm_screen_enable_altscreen(vts, 1);
        // Preserve existing terminal content when the viewport becomes
        // narrower. Without libvterm's native reflow, resize truncates the
        // right side of every populated row until new output arrives.
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
        callbacks.sb_popline = &Impl::onScrollbackPop;
        callbacks.sb_clear = &Impl::onScrollbackClear;
        vterm_screen_set_callbacks(vts, &callbacks, this);
    }

    ~Impl()
    {
        if (vt)
            vterm_free(vt);
    }

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

        for (int row = startRow; row < endRow; ++row) {
            for (int column = startColumn; column < endColumn; ++column) {
                VTermScreenCell source{};
                if (vterm_screen_get_cell(vts, {row, column}, &source))
                    screen.setCell(row, column, fromVTermCell(source));
            }
        }
    }

    static void onOutput(const char* data, size_t length, void* user)
    {
        auto& self = *static_cast<Impl*>(user);
        if (self.observer.output)
            self.observer.output(QByteArray(data, static_cast<int>(length)));
    }

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

    static int onMoveRect(VTermRect destination, VTermRect source, void* user)
    {
        auto& self = *static_cast<Impl*>(user);
        const DirtyRegion destinationRegion{
            destination.start_row, destination.end_row,
            destination.start_col, destination.end_col};
        const DirtyRegion sourceRegion{
            source.start_row, source.end_row,
            source.start_col, source.end_col};
        self.screen.moveRect(destinationRegion, sourceRegion);
        if (self.observer.damage)
            self.observer.damage(destinationRegion);
        return 1;
    }

    static int onMoveCursor(VTermPos position, VTermPos, int visible, void* user)
    {
        auto& self = *static_cast<Impl*>(user);
        self.cursorState.position = {position.row, position.col};
        self.cursorState.visible = visible != 0;
        if (self.observer.cursorChanged)
            self.observer.cursorChanged(self.cursorState);
        return 1;
    }

    static int onSetTermProperty(VTermProp property, VTermValue* value, void* user)
    {
        auto& self = *static_cast<Impl*>(user);
        bool cursorChanged = false;
        switch (property) {
        case VTERM_PROP_TITLE:
            if (value->string.str) {
                self.title = QString::fromUtf8(value->string.str,
                                               static_cast<int>(value->string.len));
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

    static int onResize(int rows, int columns, void* user)
    {
        auto& self = *static_cast<Impl*>(user);
        self.screen.resize(columns, rows);
        self.syncRegion({0, rows, 0, columns});
        return 1;
    }

    static int onScrollbackPush(int columns, const VTermScreenCell* cells,
                                int softWrapped, void* user)
    {
        auto& self = *static_cast<Impl*>(user);
        int storedColumns = columns;
        while (storedColumns > 0
               && isDefaultBlankCell(cells[storedColumns - 1])) {
            --storedColumns;
        }

        QVector<Cell>& converted =
            self.scrollback.beginPushLine(columns, storedColumns);
        for (int column = 0; column < storedColumns; ++column)
            populateCell(cells[column], converted[column]);
        self.scrollback.commitPushLine(self.nextScrollbackContinuation,
                                       softWrapped == 0);
        self.nextScrollbackContinuation = softWrapped != 0;
        // Reflow during an explicit resize can move rows into scrollback, but
        // it is not an incremental live-screen scroll. The renderer will
        // receive one full-screen damage publication when resize completes.
        if (!self.resizeInProgress && self.observer.screenScrolled)
            self.observer.screenScrolled(1);
        if (self.observer.scrollbackChanged)
            self.observer.scrollbackChanged();
        return 1;
    }

    static int onScrollbackPop(int columns, VTermScreenCell* cells, void* user)
    {
        auto& self = *static_cast<Impl*>(user);
        std::vector<Cell> converted(columns);
        if (!self.scrollback.popLine(converted.data(), columns))
            return 0;
        for (int column = 0; column < columns; ++column)
            cells[column] = toVTermCell(converted[column]);
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
    QString title;
    VTerm* vt{nullptr};
    VTermScreen* vts{nullptr};
    VTermState* state{nullptr};
    VTermScreenCallbacks callbacks{};
    bool nextScrollbackContinuation{false};
    bool resizeInProgress{false};
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

void VTAdapter::writeInput(const QByteArray& data)
{
    if (isValid())
        vterm_input_write(_impl->vt, data.constData(), data.size());
}

void VTAdapter::flushDamage()
{
    if (isValid())
        vterm_screen_flush_damage(_impl->vts);
}

void VTAdapter::resize(int columns, int rows)
{
    if (isValid()) {
        const QScopedValueRollback<bool> resizeGuard(
            _impl->resizeInProgress, true);
        vterm_set_size(_impl->vt, rows, columns);
        // libvterm's resize callback synchronizes the ScreenBuffer, but a
        // resize is not guaranteed to produce ordinary parser damage when no
        // bytes are arriving. Publish a full region explicitly so the async
        // model resize always invalidates every cached CPU/GPU row.
        if (_impl->observer.damage)
            _impl->observer.damage({0, rows, 0, columns});
    }
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

QString VTAdapter::title() const
{
    return _impl ? _impl->title : QString{};
}

} // namespace NovaTerm
