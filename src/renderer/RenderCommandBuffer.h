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
    int atlasPage{-1};
    quint64 pageGeneration{0};
    int cellColumn{-1};
    bool colorGlyph{false};
};

struct DirtyColumnSpan
{
    int startColumn{0};
    int endColumn{0};
};

struct RenderCommandRow
{
    QVector<RenderCommand> backgrounds;
    QVector<RenderCommand> contents;
    quint64 revision{0};
    quint64 atlasGeneration{0};
    quint64 contentRevision{0};
    QVector<DirtyColumnSpan> dirtySpans;
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
                    quint64 atlasGeneration = 0,
                    quint64 contentRevision = 0,
                    QVector<DirtyColumnSpan> dirtySpans = {});

    const QVector<RenderCommand>& overlays() const { return _overlays; }
    void replaceOverlays(QVector<RenderCommand> overlays);
    void rotateRowsUp(int count);

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
