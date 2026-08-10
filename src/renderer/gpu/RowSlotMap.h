/**
 * @file   RowSlotMap.h
 * @brief  可见行 ↔ GPU 槽位映射。
 *
 * 终端滚动时，可见行的内容在源数据（scrollback/活动屏幕）中可能只是
 * 偏移变化。RowSlotMap 维护 widgetRow（屏幕第几行）→ gpuSlot（GPU
 * 缓冲第几个槽位）的映射，当某行 identity 未变时复用原槽位，仅
 * 改变 yTransform，避免重新上传整行顶点数据。
 */
#pragma once

#include <QHash>
#include <QVector>
#include <QtGlobal>

namespace NovaTerm {

/**
 * @brief 比较缓存的行身份与当前行身份，返回需要重建的行号。
 *
 * 行 identity 变化但 widgetRow 不变时，该行的 GPU 槽位内容已过时，
 * 需要重新上传顶点。已标记为脏（dirtyRows=true）的行调用方会单独
 * 处理，此处不重复返回。
 *
 * @param cachedIdentities  上次映射时各行的 identity。
 * @param currentIdentities 本次各行的 identity。
 * @param dirtyRows         已被标记为脏的行（这些行不重复返回）。
 * @return 需要重建的行号列表。
 */
QVector<int> rowsNeedingRebuildAfterMapping(
    const QVector<quint64>& cachedIdentities,
    const QVector<quint64>& currentIdentities,
    const QVector<bool>& dirtyRows);

// 可见行身份：唯一标识一行内容来源。同一 sourceId+sourceVersion+wrapIndex
// 的行内容相同，可复用 GPU 槽位。activeScreen 区分活动屏与 scrollback。
struct VisibleRowIdentity
{
    quint64 sourceId{0};        // 源行 ID（scrollback chunk + 偏移哈希）
    quint64 sourceVersion{0};   // 源行内容版本
    qsizetype wrapIndex{0};     // 软换行后的第几段（0=首段）
    bool activeScreen{false};   // 是否来自活动屏幕（true）而非 scrollback

    friend bool operator==(const VisibleRowIdentity& a,
                           const VisibleRowIdentity& b)
    {
        return a.sourceId == b.sourceId
            && a.sourceVersion == b.sourceVersion
            && a.wrapIndex == b.wrapIndex
            && a.activeScreen == b.activeScreen;
    }
};

inline size_t qHash(const VisibleRowIdentity& id, size_t seed = 0) noexcept
{
    return qHashMulti(seed, id.sourceId, id.sourceVersion, id.wrapIndex,
                      id.activeScreen);
}

// 单行的映射结果：identity + widgetRow + gpuSlot + yTransform。
struct RowPlacement
{
    VisibleRowIdentity identity;
    int widgetRow{-1};        // 屏幕行号（0=最上方可见行）
    int gpuSlot{-1};          // GPU 缓冲槽位号
    float yTransform{0};      // 该行在 GPU 中的 y 偏移（像素）
    quint64 mappingRevision{0};  // 本次映射的 revision
    bool reused{false};        // 是否复用了上一帧的槽位
};

// 一次 update() 的结果：新的全部 placements + 增量信息。
struct RowSlotUpdate
{
    QVector<RowPlacement> placements;  // 新映射（按 widgetRow 顺序）
    QVector<int> enteringWidgetRows;   // 新进入的行（需上传顶点）
    QVector<int> retiredSlots;          // 已退役的槽位（可释放或复用）
    int reusedRows{0};                  // 复用的行数（性能指标）
    bool fullRemap{false};              // 是否发生了全量重映射
};

// 可见行 ↔ GPU 槽位映射器。单线程使用。
class RowSlotMap
{
public:
    /**
     * @brief 用新的可见行列表更新映射。identity 相同的行复用原槽位，
     *        仅更新 yTransform；identity 变化的行分配新槽位（优先
     *        复用已释放的）。forceFull=true 时强制全量重映射。
     * @param rows       新的可见行 identity 列表。
     * @param rowHeight  单行像素高度，用于计算 yTransform。
     * @param forceFull  是否强制全量重映射。
     * @return 更新结果，含新 placements 与增量信息。
     */
    RowSlotUpdate update(const QVector<VisibleRowIdentity>& rows,
                         float rowHeight, bool forceFull = false);

    /**
     * @brief 重置为顺序映射：第 i 行 → 第 i 个槽位，identity 全部置空。
     *        用于初始化或视口尺寸变化后的全量重建。
     */
    void resetSequential(int rows, float rowHeight);

    /**
     * @brief 把所有 placements 向上滚动 count 行：顶部 count 行被丢弃，
     *        底部 count 行变为新行。用于活动屏幕上滚时同步 GPU 槽位。
     */
    void rotateRowsUp(int count, float rowHeight);
    void reset();
    quint64 mappingRevision() const { return _mappingRevision; }
    int capacity() const { return _capacity; }
    int slotForWidgetRow(int widgetRow) const;

    /**
     * @brief 校验当前映射是否为合法排列：每行恰好对应一个不重复的槽位。
     *        用于调试与断言。
     */
    bool isValidPermutation(int rows) const;
    const QVector<RowPlacement>& placements() const { return _placements; }

private:
    // 分配一个槽位：优先从 freeSlots 复用，否则扩展容量。
    int allocateSlot(QVector<int>& freeSlots);

    int _capacity{0};            // 已分配过的最大槽位号 +1
    quint64 _mappingRevision{0};  // 映射版本号，每次 update/rotate 递增
    QVector<RowPlacement> _placements;
};

} // namespace NovaTerm
