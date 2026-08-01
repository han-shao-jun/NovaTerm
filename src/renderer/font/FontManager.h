#pragma once

#include "renderer/glyph/GlyphTypes.h"

#include <QFont>
#include <QHash>
#include <QRawFont>
#include <QStringList>

namespace NovaTerm {

struct FontSelection
{
    QFont font;
    FontFaceId faceId{0};
    int fallbackIndex{0};
    bool completeCoverage{false};
};

class FontManager
{
public:
    explicit FontManager(QFont primary = {});

    void setPrimaryFont(const QFont& font);
    void setFallbackFamilies(QStringList families);
    const QFont& primaryFont() const { return _primary; }
    quint64 generation() const { return _generation; }
    const QStringList& fallbackFamilies() const { return _fallbackFamilies; }

    FontSelection select(const QString& cluster, bool bold = false,
                         bool italic = false) const;
    GlyphKey makeKey(const QString& cluster, bool bold, bool italic,
                     int cellSpan, qreal effectiveScale,
                     GlyphRenderMode mode = GlyphRenderMode::Grayscale) const;

private:
    static bool covers(const QRawFont& raw, const QString& cluster);
    static FontFaceId idFor(const QFont& font);
    QList<QFont> candidates(bool bold, bool italic) const;

    QFont _primary;
    QStringList _fallbackFamilies;
    quint64 _generation{1};
};

} // namespace NovaTerm
