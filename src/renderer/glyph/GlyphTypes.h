#pragma once

#include <QImage>
#include <QRect>
#include <QString>
#include <QtGlobal>

namespace NovaTerm {

using FontFaceId = quint64;

enum class GlyphRenderMode : quint8 { Grayscale, Color };
enum class GlyphPixelFormat : quint8 { Alpha8, Rgba8 };

struct GlyphKey
{
    FontFaceId faceId{0};
    quint64 fontGeneration{0};
    QString cluster;
    int pixelSize{0};
    int scale1024{1024};
    int weight{400};
    int cellSpan{1};
    int fallbackIndex{0};
    quint64 featuresIdentity{0};
    bool italic{false};
    bool syntheticBold{false};
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
    quint64 sourceGeneration{0};
    QString diagnostic;
};

struct GlyphLocation
{
    int pageId{-1};
    quint64 pageGeneration{0};
    QRect pixelRect;
    QRectF logicalRect;
    GlyphPixelFormat format{GlyphPixelFormat::Alpha8};

    bool isValid() const { return pageId >= 0 && !pixelRect.isEmpty(); }
};

} // namespace NovaTerm
