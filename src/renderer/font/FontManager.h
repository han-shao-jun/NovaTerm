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
#include <QRawFont>
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

private:
    // 检查 QRawFont 是否能覆盖该簇。跳过 ZWJ 与变体选择符等
    // 无独立字形的码点（它们依赖基础码点 shaping）。
    static bool covers(const QRawFont& raw, const QString& cluster);
    // 由 QFont 计算 FontFaceId（哈希）。
    static FontFaceId idFor(const QFont& font);
    // 构造候选字体列表：主字体 → 回退列表 → Qt 平台回退。
    QList<QFont> candidates(bool bold, bool italic) const;

    QFont _primary;
    QStringList _fallbackFamilies;
    quint64 _generation{1};
};

} // namespace NovaTerm
