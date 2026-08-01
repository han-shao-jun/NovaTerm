#pragma once

#include <QColor>
#include <QRectF>
#include <QVector>

#include <cstdint>

namespace NovaTerm {

enum class RenderCommandType : uint8_t
{
    BackgroundRect,
    GlyphInstance,
    Underline,
    Strike,
    Cursor,
    SelectionOverlay,
    HyperlinkOverlay,
    SearchOverlay
};

struct RenderCommand
{
    RenderCommandType type{RenderCommandType::BackgroundRect};
    QRectF rect;
    QRectF uvRect;
    QColor color;
};

struct RenderCommandRow
{
    QVector<RenderCommand> backgrounds;
    QVector<RenderCommand> contents;
    quint64 revision{0};
    quint64 atlasGeneration{0};
};

class RenderCommandBuffer
{
public:
    void resize(int rows, int columns);
    int rows() const { return _rows; }
    int columns() const { return _columns; }

    const RenderCommandRow& row(int index) const;
    void replaceRow(int index,
                    QVector<RenderCommand> backgrounds,
                    QVector<RenderCommand> contents,
                    quint64 atlasGeneration = 0);

    const QVector<RenderCommand>& overlays() const { return _overlays; }
    void replaceOverlays(QVector<RenderCommand> overlays);

    qsizetype commandCount() const;
    bool rowsUseAtlasGeneration(quint64 atlasGeneration) const;
    quint64 revision() const { return _revision; }

private:
    int _rows{0};
    int _columns{0};
    QVector<RenderCommandRow> _rowCommands;
    QVector<RenderCommand> _overlays;
    quint64 _revision{0};
};

} // namespace NovaTerm
