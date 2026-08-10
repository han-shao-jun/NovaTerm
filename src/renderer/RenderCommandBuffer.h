/**
 * @file   RenderCommandBuffer.h
 * @brief  渲染命令缓冲。
 *
 * TerminalRenderer 把每行 Cell 转换为 RenderCommand 序列存入本缓冲，
 * 再由 GPU 批量绘制。缓冲按行组织，每行拆分为背景层（BackgroundRect）
 * 与内容层（GlyphInstance/Underline/Strike 等）；overlay（光标、选区、
 * 搜索高亮）独立于行存储。
 */
#pragma once

#include <QColor>
#include <QRectF>
#include <QVector>

#include <cstdint>

namespace NovaTerm {

// 渲染命令类型。
enum class RenderCommandType : uint8_t
{
    BackgroundRect,       // 单元格背景矩形
    GlyphInstance,        // 字形实例（引用 GlyphAtlas 中的纹理区域）
    Underline,            // 下划线（含 Single/Double/Curly）
    Strike,               // 删除线
    Cursor,               // 光标
    SelectionOverlay,     // 选区高亮叠加
    HyperlinkOverlay,     // 超链接高亮
    SearchOverlay         // 搜索命中高亮
};

// 单条渲染命令。并非所有字段对所有类型有意义。
struct RenderCommand
{
    RenderCommandType type{RenderCommandType::BackgroundRect};
    QRectF rect;            // 屏幕坐标（像素）
    QRectF uvRect;          // 字形在 GlyphAtlas 中的 UV
    QColor color;           // 着色（前景/背景/叠加颜色）
    int atlasPage{-1};      // 字形所在的 atlas 页索引
    quint64 pageGeneration{0};  // 页生成代际，用于检测 atlas 是否已被重建
    int cellColumn{-1};     // 该命令对应的 Cell 列号（用于列级脏判定）
    bool colorGlyph{false}; // 是否为彩色字形（如 emoji），不走 tinting
};

// 行内的脏列区间（半开区间 [start, end)），用于缩小 GPU 上传范围。
struct DirtyColumnSpan
{
    int startColumn{0};
    int endColumn{0};
};

// 单行的渲染命令集合。backgrounds 在 contents 之前绘制。
struct RenderCommandRow
{
    QVector<RenderCommand> backgrounds;
    QVector<RenderCommand> contents;
    quint64 revision{0};            // 行模型版本
    quint64 atlasGeneration{0};     // 行字形所基于的 atlas 代际
    quint64 contentRevision{0};     // 行内容版本（不含背景变化）
    QVector<DirtyColumnSpan> dirtySpans;
};

// 渲染命令缓冲。GUI 线程独占，无需加锁。
class RenderCommandBuffer
{
public:
    void resize(int rows, int columns);
    int rows() const { return _rows; }
    int columns() const { return _columns; }

    const RenderCommandRow& row(int index) const;

    /**
     * @brief 替换指定行的渲染命令。
     * @param index 行号。
     * @param backgrounds 背景层命令。
     * @param contents 内容层命令。
     * @param atlasGeneration 该行字形所基于的 atlas 代际。
     * @param contentRevision 该行内容版本。
     * @param dirtySpans 该行脏列区间。
     */
    void replaceRow(int index,
                    QVector<RenderCommand> backgrounds,
                    QVector<RenderCommand> contents,
                    quint64 atlasGeneration = 0,
                    quint64 contentRevision = 0,
                    QVector<DirtyColumnSpan> dirtySpans = {});

    const QVector<RenderCommand>& overlays() const { return _overlays; }
    void replaceOverlays(QVector<RenderCommand> overlays);

    /**
     * @brief 把所有行向上滚动 count 行：顶部 count 行被丢弃，
     *        底部补 count 个空行。用于活动屏幕上滚时同步命令缓冲。
     */
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
