/**
 * @file   GlyphTypes.h
 * @brief  字形相关基础类型。
 *
 * 定义 GlyphKey（字形唯一标识）、GlyphBitmap（栅格化结果）与
 * GlyphLocation（atlas 中的位置）等数据结构，被 FontManager、
 * GlyphRasterizer、GlyphCache、GlyphAtlas 共享。
 */
#pragma once

#include <QImage>
#include <QRect>
#include <QString>
#include <QtGlobal>

namespace NovaTerm {

// 字体面 ID（由 FontManager 计算的哈希），用于唯一标识一个字体配置。
using FontFaceId = quint64;

// 渲染模式：灰度（普通字形）或彩色（emoji 等）。
enum class GlyphRenderMode : quint8 { Grayscale, Color };
// 像素格式：Alpha8（灰度字形）或 Rgba8（彩色字形）。
enum class GlyphPixelFormat : quint8 { Alpha8, Rgba8 };

// 字形唯一标识。同一 GlyphKey 在同一字体配置下必然产生相同栅格化结果，
// 因此可作为 GlyphCache 的键。fontGeneration 用于在字体变更时整体失效。
struct GlyphKey
{
    FontFaceId faceId{0};
    quint64 fontGeneration{0};
    QString cluster;             // 待绘制的字形簇（可能含组合序列）
    int pixelSize{0};
    int scale1024{1024};          // 1/1024 定点缩放，避免浮点键比较
    int weight{400};
    int cellSpan{1};             // 占用的 Cell 宽度（1 或 2）
    int fallbackIndex{0};
    quint64 featuresIdentity{0};  // OpenType feature 集合的哈希
    bool italic{false};
    bool syntheticBold{false};    // 字体不支持粗体时的合成粗体
    bool syntheticItalic{false};
    GlyphRenderMode renderMode{GlyphRenderMode::Grayscale};
    GlyphPixelFormat format{GlyphPixelFormat::Alpha8};

    friend bool operator==(const GlyphKey& a, const GlyphKey& b)
    {
        return a.faceId == b.faceId
            && a.fontGeneration == b.fontGeneration
            && a.cluster == b.cluster
            && a.pixelSize == b.pixelSize
            && a.scale1024 == b.scale1024
            && a.weight == b.weight
            && a.cellSpan == b.cellSpan
            && a.fallbackIndex == b.fallbackIndex
            && a.featuresIdentity == b.featuresIdentity
            && a.italic == b.italic
            && a.syntheticBold == b.syntheticBold
            && a.syntheticItalic == b.syntheticItalic
            && a.renderMode == b.renderMode
            && a.format == b.format;
    }
};

inline size_t qHash(const GlyphKey& key, size_t seed = 0) noexcept
{
    seed = qHashMulti(seed, key.faceId, key.fontGeneration, key.cluster,
                      key.pixelSize, key.scale1024, key.weight, key.cellSpan,
                      key.fallbackIndex, key.featuresIdentity, key.italic,
                      key.syntheticBold, key.syntheticItalic,
                      quint8(key.renderMode), quint8(key.format));
    return seed;
}

// 字形栅格化结果：包含位图与几何度量（bearing/advance/baseline）。
struct GlyphBitmap
{
    GlyphKey key;
    QImage image;
    QRectF logicalRect;
    qreal bearingX{0};
    qreal bearingY{0};
    qreal advance{0};
    qreal baseline{0};
    int cellSpan{1};
    quint64 sourceGeneration{0};  // 栅格化时的字体 generation
    QString diagnostic;
};

// 字形在 GlyphAtlas 中的位置：页 ID + 页内像素矩形。
struct GlyphLocation
{
    int pageId{-1};
    quint64 pageGeneration{0};   // 页生成代际，用于检测 atlas 是否已重建
    QRect pixelRect;
    QRectF logicalRect;
    GlyphPixelFormat format{GlyphPixelFormat::Alpha8};

    bool isValid() const { return pageId >= 0 && !pixelRect.isEmpty(); }
};

} // namespace NovaTerm
