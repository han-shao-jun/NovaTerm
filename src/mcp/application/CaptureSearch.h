/**
 * @file CaptureSearch.h
 * @brief 在已授权的有界 capture 上做统一文本搜索（纯函数）。
 *
 * 偏移是 UTF-8 字节；按 source 分别编号行号；每 4 KiB 检查中断并保留
 * query 长度重叠，避免漏掉块边界命中。调用方负责 capture 绑定/到期校验。
 */
#pragma once

#include "CaptureStore.h"

#include <QString>
#include <QVector>

#include <functional>

namespace NovaTerm::Mcp {

struct SearchMatch
{
    QString source;
    int lineIndex{0};
    int startByte{0};
    int endByte{0};
};

struct CaptureSearchResult
{
    QVector<SearchMatch> matches;
    bool limited{false};
    bool interrupted{false};
};

[[nodiscard]] CaptureSearchResult searchCapture(const CaptureRecord& capture,
    const QString& query, bool caseSensitive, const QString& scope, int limit,
    const std::function<bool()>& stop = {});

} // namespace NovaTerm::Mcp
