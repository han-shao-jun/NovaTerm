/**
 * @file   FontManager.cpp
 * @brief  字体选择与回退管理实现。
 *
 * 详见 FontManager.h。本文件实现候选字体构造、覆盖判定与 GlyphKey 生成。
 */
#include "FontManager.h"

#include <QFontDatabase>

#include <algorithm>

namespace NovaTerm {

FontManager::FontManager(QFont primary)
    : _primary(std::move(primary))
{
    // 未指定主字体时退化为系统等宽字体。
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
    invalidateCaches();
}

void FontManager::setFallbackFamilies(QStringList families)
{
    families.removeDuplicates();
    if (_fallbackFamilies == families)
        return;
    _fallbackFamilies = std::move(families);
    ++_generation;
    invalidateCaches();
}

void FontManager::invalidateCaches()
{
    _selectionCache.clear();
    for (bool& cached : _candidateCached)
        cached = false;
}

bool FontManager::covers(const QRawFont& raw, const QString& cluster)
{
    if (!raw.isValid() || cluster.isEmpty())
        return false;
    const auto scalars = cluster.toUcs4();
    for (char32_t scalar : scalars) {
        // ZWJ（U+200D）与变体选择符（U+FE00..U+FE0F）在某些字体中
        // 没有独立字形，应跳过；覆盖性由基础码点决定。
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
    // Qt 平台回退放在最后：保留请求的栅格尺寸与样式，即使最终解析的
    // 字体族不同。用于捕获前两者都未覆盖的码点（如某些 emoji）。
    QFont platform(primary);
    platform.setStyleStrategy(QFont::PreferDefault);
    result.push_back(platform);
    return result;
}

const FontManager::CandidateSet& FontManager::candidateSet(bool bold,
                                                           bool italic) const
{
    const int slot = (bold ? 1 : 0) | (italic ? 2 : 0);
    CandidateSet& set = _candidateCache[slot];
    if (!_candidateCached[slot]) {
        set.fonts = candidates(bold, italic);
        set.rawFonts.clear();
        set.rawFonts.reserve(set.fonts.size());
        // QRawFont::fromFont 是重操作，此处每样式只做一次并缓存；coverage
        // 探测（select）复用这些 QRawFont，不再每 Cell 重建。
        for (const QFont& font : std::as_const(set.fonts))
            set.rawFonts.push_back(QRawFont::fromFont(font));
        _candidateCached[slot] = true;
    }
    return set;
}

FontSelection FontManager::select(const QString& cluster, bool bold,
                                  bool italic) const
{
    // coverage 缓存键：cluster + 样式位。ASCII/CJK 等常见簇在稳态下命中，
    // 完全跳过候选构造与 QRawFont 探测（P5 §5.2）。
    QString cacheKey = cluster;
    cacheKey += QChar(u'\x1');
    cacheKey += QChar(ushort((bold ? 1 : 0) | (italic ? 2 : 0)));
    if (const auto it = _selectionCache.constFind(cacheKey);
        it != _selectionCache.constEnd()) {
        return it.value();
    }

    // 缓存键来自外部输入（远端可发任意码点），设上限防止病理输入下无界增长。
    // coverage 结果可廉价重算，满了整体清空即可（不需 LRU）。8192 条足够覆盖
    // 常规终端字符集，单条仅一个 QFont + 少量整数。
    constexpr int MaxSelectionCacheEntries = 8192;
    if (_selectionCache.size() >= MaxSelectionCacheEntries)
        _selectionCache.clear();

    const CandidateSet& set = candidateSet(bold, italic);
    FontSelection result;
    bool found = false;
    for (int index = 0; index < set.fonts.size(); ++index) {
        if (covers(set.rawFonts[index], cluster)) {
            result = {set.fonts[index], idFor(set.fonts[index]), index, true};
            found = true;
            break;
        }
    }
    if (!found) {
        // 全部候选均无法覆盖：退化为首个候选，仍返回结果让渲染层绘制
        // （可能是豆腐字），completeCoverage=false 供调用方诊断。
        result = {set.fonts.front(), idFor(set.fonts.front()), 0, false};
    }
    _selectionCache.insert(cacheKey, result);
    return result;
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
    // 缩放以 1/1024 定点存储，避免浮点键比较误差。
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
