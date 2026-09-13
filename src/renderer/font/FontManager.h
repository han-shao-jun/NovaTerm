/**
 * @file   FontManager.h
 * @brief  字体选择与回退管理。
 *
 * 终端需要为每个字形簇（cluster）选择一个能覆盖它的字体。FontManager
 * 按主字体 → 用户配置回退列表 → Qt 平台回退的顺序尝试，命中首个
 * 覆盖该簇的字体。所有选择无状态，结果通过 FontSelection 返回。
 */
#pragma once

#include "renderer/glyph/GlyphTypes.h"

#include <QFont>
#include <QHash>
#include <QList>
#include <QRawFont>
#include <QString>
#include <QStringList>

namespace NovaTerm {

// 一次字体选择结果。completeCoverage=false 表示所有候选字体均无法
// 完整覆盖该簇，已退化为首个候选（仍可绘制但可能显示豆腐字）。
struct FontSelection
{
    QFont font;
    FontFaceId faceId{0};
    int fallbackIndex{0};
    bool completeCoverage{false};
};

// 字体选择器。无状态，可跨线程调用；但内部 QFont 实例非 const，
// 故建议单实例独占使用。
class FontManager
{
public:
    explicit FontManager(QFont primary = {});

    /**
     * @brief 设置主字体。会递增 generation，使 GlyphCache 中所有
     *        旧字形失效。
     */
    void setPrimaryFont(const QFont& font);

    /**
     * @brief 设置回退字体族列表。会递增 generation。
     */
    void setFallbackFamilies(QStringList families);
    const QFont& primaryFont() const { return _primary; }
    quint64 generation() const { return _generation; }
    const QStringList& fallbackFamilies() const { return _fallbackFamilies; }

    /**
     * @brief 为给定簇选择最合适的字体。
     * @param cluster 待绘制的字形簇（可能含组合序列）。
     * @param bold 是否需要粗体。
     * @param italic 是否需要斜体。
     * @return 字体选择结果。
     */
    FontSelection select(const QString& cluster, bool bold = false,
                         bool italic = false) const;

    /**
     * @brief 构造 GlyphKey，用于在 GlyphCache 中查找或注册字形。
     * @param cluster 待绘制的字形簇。
     * @param bold 粗体。
     * @param italic 斜体。
     * @param cellSpan 该字形占用的 Cell 宽度（1 或 2）。
     * @param effectiveScale 实际渲染缩放（用于亚像素字号）。
     * @param mode 渲染模式（灰度/彩色）。
     * @return GlyphKey。
     */
    GlyphKey makeKey(const QString& cluster, bool bold, bool italic,
                     int cellSpan, qreal effectiveScale,
                     GlyphRenderMode mode = GlyphRenderMode::Grayscale) const;

    /// `makeKeyAndSelection()` 的结果：一次字体选择同时产出 GlyphKey。
    struct GlyphKeySelection
    {
        GlyphKey key;
        FontSelection selection;
    };

    /**
     * @brief 一次字体选择同时产出 GlyphKey 与其 FontSelection。
     *
     * @note 这是渲染路径的窄接口：cache miss 时渲染层直接用返回的 selection
     *       发起栅格化，不必再调用一次 `select()`。旧实现里 `ensureGlyph()`
     *       先 `makeKey()`（内部 select）再单独 `select()`，同一簇每帧做两次
     *       字体选择，是 glyph 稳态的主要冗余。
     */
    [[nodiscard]] GlyphKeySelection makeKeyAndSelection(
        const QString& cluster, bool bold, bool italic, int cellSpan,
        qreal effectiveScale,
        GlyphRenderMode mode = GlyphRenderMode::Grayscale) const;

    /// 诊断计数：`select()` 被查询的次数（含命中缓存的调用）。
    [[nodiscard]] quint64 selectionQueryCount() const
    {
        return _selectionQueries;
    }

    /// 诊断计数：真正执行候选字体 coverage 探测的次数（缓存命中不计数）。
    [[nodiscard]] quint64 selectionProbeCount() const
    {
        return _selectionProbes;
    }

private:
    // 检查 QRawFont 是否能覆盖该簇。跳过 ZWJ 与变体选择符等
    // 无独立字形的码点（它们依赖基础码点 shaping）。
    static bool covers(const QRawFont& raw, const QString& cluster);
    // 由 QFont 计算 FontFaceId（哈希）。
    static FontFaceId idFor(const QFont& font);
    // 构造候选字体列表：主字体 → 回退列表 → Qt 平台回退。
    QList<QFont> candidates(bool bold, bool italic) const;
    // 不走 ASCII 直连缓存的通用选择路径（QHash 键 + coverage 探测）。
    FontSelection selectUncached(const QString& cluster, bool bold,
                                 bool italic) const;
    // 由已选中的字体构造 GlyphKey（ASCII 缓存与通用路径共用）。
    GlyphKey buildKey(const FontSelection& selection, const QString& cluster,
                      int cellSpan, qreal effectiveScale,
                      GlyphRenderMode mode) const;

    // 一组候选字体及其 QRawFont。QRawFont::fromFont 是重操作，按
    // (bold,italic) 缓存，避免每 Cell 重建候选与探测字体（P5 §5.2）。
    struct CandidateSet
    {
        QList<QFont> fonts;
        QList<QRawFont> rawFonts;
    };
    // 取给定样式的候选集，缓存未命中时构造。generation 变化时整表已清空。
    const CandidateSet& candidateSet(bool bold, bool italic) const;
    // 清空 coverage/候选缓存。setPrimaryFont/setFallbackFamilies 调用。
    void invalidateCaches();

    QFont _primary;
    QStringList _fallbackFamilies;
    quint64 _generation{1};

    // ── coverage 查询缓存（P5 §5.2：避免每 Cell 重复探测字体）──
    // 键为 cluster + 样式位；值为该簇的选择结果。随 generation 失效。
    // mutable：select()/makeKey() 语义上是查询（const），缓存是实现细节。
    mutable QHash<QString, FontSelection> _selectionCache;
    // 候选集按样式缓存：索引 0=常规,1=bold,2=italic,3=bold+italic。
    mutable CandidateSet _candidateCache[4];
    mutable bool _candidateCached[4]{false, false, false, false};

    // ── ASCII 直连缓存 ────────────────────────────────────────
    // 终端稳态绝大多数 Cell 是单个 ASCII 码点：用 (码点, 样式位) 直接寻址即可
    // 命中，省掉"构造 QString 键 + QHash 查找"，也避免 ASCII 簇反复走 coverage
    // 探测。generation 变化时条目自动失效（比较 generation，不清表）。
    static constexpr int AsciiCacheCodepoints = 128;
    struct AsciiCacheEntry
    {
        FontSelection selection;
        quint64 generation{0};
        bool valid{false};
    };
    mutable AsciiCacheEntry _asciiCache[AsciiCacheCodepoints * 4];

    // 诊断计数，供 RendererP5Tests 断言"一次选择不重复探测"。
    mutable quint64 _selectionQueries{0};
    mutable quint64 _selectionProbes{0};
};

} // namespace NovaTerm
