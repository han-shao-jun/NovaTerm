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
     * @brief 取得可写行，用于就地重建命令并复用目标向量的容量。
     * @param index 行号。
     * @return 该行的可写引用；越界时返回缓冲内部的空行（与 row() 的断言
     *         不同，供重建路径在 resize 竞态下安全降级，写入即丢弃）。
     * @note 返回的行仍是上一帧的命令。重建方应先把旧命令移出（swap 到自己的
     *        scratch）再写入，避免边读边写同一个向量，同时把容量留给新帧。
     */
    RenderCommandRow& mutableRow(int index);

    /**
     * @brief 就地重建收尾：更新 atlas 代际并把行标记为新版本。
     * @param index 行号；越界（例如 resize 竞态）时忽略。
     * @param atlasGeneration 该行字形所基于的 atlas 代际。
     * @note 命名命令数据由 mutableRow() 就地写入，本接口只负责元数据，
     *       因此不会移动命令向量、也不会丢掉它们的容量。
     */
    void finishRow(int index, quint64 atlasGeneration = 0);

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
    RenderCommandRow _scratchRow; ///< mutableRow() 越界时的写入落点
    quint64 _revision{0};
};

/**
 * @brief 按列增量重建一行的命令：保留未脏列的旧命令 + 脏列的新命令。
 *
 * @param oldBackgrounds 上一帧背景命令，必须按 cellColumn 升序。
 * @param oldContents 上一帧内容命令，必须按 cellColumn 升序。
 * @param columns 该行列数。
 * @param dirtySpans 本次脏列区间（升序、已合并）；replaceAll 为 true 时忽略。
 * @param replaceAll 为 true 表示整行重建，丢弃全部旧命令。
 * @param generate 生成回调 `generate(column, backgrounds, contents)`，仅在列脏时
 *        调用一次；把该列的新命令追加到两个输出向量（不得写入其他列）。
 * @param outBackgrounds 输出背景序列；调用前应 clear() 以复用容量。
 * @param outContents 输出内容序列；调用前应 clear() 以复用容量。
 *
 * @note 结果按 cellColumn 升序，同列时旧命令在前（与旧实现的 stable_sort 等价），
 *       但只做一次 O(旧 + 新 + 列数) 的线性扫描：不需要排序、排序临时缓冲，
 *       也不重复生成命令。cellColumn < 0 的行级命令始终保留并排在最前。
 * @note 这是纯 CPU 模板函数，供 RendererP3Tests 直接验证增量合并语义。
 */
template <typename Generate>
void mergeRowCommandsIncremental(
    const QVector<RenderCommand>& oldBackgrounds,
    const QVector<RenderCommand>& oldContents, int columns,
    const QVector<DirtyColumnSpan>& dirtySpans, bool replaceAll,
    Generate&& generate, QVector<RenderCommand>& outBackgrounds,
    QVector<RenderCommand>& outContents)
{
    // 同列旧命令整组搬运；cursor 只前进不回退，故整体是线性扫描。
    const auto appendColumn = [](const QVector<RenderCommand>& source,
                                 qsizetype& cursor, int column,
                                 QVector<RenderCommand>& target) {
        while (cursor < source.size() && source[cursor].cellColumn < column)
            ++cursor;
        while (cursor < source.size() && source[cursor].cellColumn == column)
            target.push_back(source[cursor++]);
    };
    const auto dropUpToColumn = [](const QVector<RenderCommand>& source,
                                   qsizetype& cursor, int column) {
        while (cursor < source.size() && source[cursor].cellColumn <= column)
            ++cursor;
    };

    qsizetype backgroundCursor = 0;
    qsizetype contentCursor = 0;
    if (!replaceAll) {
        // 行级命令（cellColumn < 0）不属于任何列，原样保留在最前。
        while (backgroundCursor < oldBackgrounds.size()
               && oldBackgrounds[backgroundCursor].cellColumn < 0)
            outBackgrounds.push_back(oldBackgrounds[backgroundCursor++]);
        while (contentCursor < oldContents.size()
               && oldContents[contentCursor].cellColumn < 0)
            outContents.push_back(oldContents[contentCursor++]);
    }

    qsizetype spanIndex = 0;
    for (int column = 0; column < columns; ++column) {
        while (spanIndex < dirtySpans.size()
               && column >= dirtySpans[spanIndex].endColumn)
            ++spanIndex;
        const bool dirty = replaceAll
            || (spanIndex < dirtySpans.size()
                && column >= dirtySpans[spanIndex].startColumn
                && column < dirtySpans[spanIndex].endColumn);
        if (dirty) {
            generate(column, outBackgrounds, outContents);
            dropUpToColumn(oldBackgrounds, backgroundCursor, column);
            dropUpToColumn(oldContents, contentCursor, column);
            continue;
        }
        appendColumn(oldBackgrounds, backgroundCursor, column, outBackgrounds);
        appendColumn(oldContents, contentCursor, column, outContents);
    }

    if (replaceAll)
        return;
    // 列宽之外的旧命令（例如终端缩窄后遗留）原样追加在末尾，避免静默丢失；
    // 脏列的命令已被 dropUpToColumn 跳过，不会在这里复活。
    while (backgroundCursor < oldBackgrounds.size())
        outBackgrounds.push_back(oldBackgrounds[backgroundCursor++]);
    while (contentCursor < oldContents.size())
        outContents.push_back(oldContents[contentCursor++]);
}

} // namespace NovaTerm
