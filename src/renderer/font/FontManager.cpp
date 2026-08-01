#include "FontManager.h"

#include <QFontDatabase>

#include <algorithm>

namespace NovaTerm {

FontManager::FontManager(QFont primary)
    : _primary(std::move(primary))
{
    if (_primary.family().isEmpty()) {
        _primary = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        _primary.setStyleHint(QFont::Monospace);
    }
}

void FontManager::setPrimaryFont(const QFont& font)
{
    if (_primary == font)
        return;
    _primary = font;
    ++_generation;
}

void FontManager::setFallbackFamilies(QStringList families)
{
    families.removeDuplicates();
    if (_fallbackFamilies == families)
        return;
    _fallbackFamilies = std::move(families);
    ++_generation;
}

bool FontManager::covers(const QRawFont& raw, const QString& cluster)
{
    if (!raw.isValid() || cluster.isEmpty())
        return false;
    const auto scalars = cluster.toUcs4();
    for (char32_t scalar : scalars) {
        // Joiners, variation selectors and combining marks may intentionally
        // have no standalone glyph in a face; shaping coverage is determined
        // by the base scalars.
        if (scalar == 0x200d || (scalar >= 0xfe00 && scalar <= 0xfe0f))
            continue;
        if (!raw.supportsCharacter(uint(scalar)))
            return false;
    }
    return true;
}

FontFaceId FontManager::idFor(const QFont& font)
{
    return FontFaceId(qHashMulti(size_t(0), font.family(), font.styleName(),
                                 font.weight(), font.italic(), font.pixelSize(),
                                 font.pointSizeF()));
}

QList<QFont> FontManager::candidates(bool bold, bool italic) const
{
    QList<QFont> result;
    QFont primary = _primary;
    primary.setBold(bold);
    primary.setItalic(italic);
    result.push_back(primary);
    for (const QString& family : _fallbackFamilies) {
        QFont fallback(primary);
        fallback.setFamily(family);
        result.push_back(fallback);
    }
    // Qt's platform fallback is deliberately last and retains the requested
    // grid size/style even when its resolved family differs.
    QFont platform(primary);
    platform.setStyleStrategy(QFont::PreferDefault);
    result.push_back(platform);
    return result;
}

FontSelection FontManager::select(const QString& cluster, bool bold,
                                  bool italic) const
{
    const QList<QFont> fonts = candidates(bold, italic);
    for (int index = 0; index < fonts.size(); ++index) {
        const QRawFont raw = QRawFont::fromFont(fonts[index]);
        if (covers(raw, cluster))
            return {fonts[index], idFor(fonts[index]), index, true};
    }
    return {fonts.front(), idFor(fonts.front()), 0, false};
}

GlyphKey FontManager::makeKey(const QString& cluster, bool bold, bool italic,
                              int cellSpan, qreal effectiveScale,
                              GlyphRenderMode mode) const
{
    const FontSelection selection = select(cluster, bold, italic);
    GlyphKey key;
    key.faceId = selection.faceId;
    key.fontGeneration = _generation;
    key.cluster = cluster;
    key.pixelSize = selection.font.pixelSize() > 0
        ? selection.font.pixelSize()
        : qRound(selection.font.pointSizeF());
    key.scale1024 = qRound(effectiveScale * 1024.0);
    key.weight = selection.font.weight();
    key.italic = selection.font.italic();
    key.cellSpan = std::max(1, cellSpan);
    key.fallbackIndex = selection.fallbackIndex;
    key.renderMode = mode;
    key.format = mode == GlyphRenderMode::Color
        ? GlyphPixelFormat::Rgba8 : GlyphPixelFormat::Alpha8;
    return key;
}

} // namespace NovaTerm
