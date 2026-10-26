/** @file ObservationService.cpp @brief 统一观察存储与兼容读取用例实现。 */
#include "ObservationService.h"
#include "McpProtocol.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QTimeZone>

#include <algorithm>
#include <chrono>

namespace NovaTerm::Mcp {

ObservationService::ObservationService(std::function<qint64()> clock,
    std::function<qint64()> wallClock)
    : _snapshots(clock), _clock(std::move(clock)),
      _wallClock(std::move(wallClock))
{
}

std::shared_ptr<const ObservationService::Snapshot> ObservationService::snapshot(
    const QString& session, const QString& epoch, const Loader& load, bool* reused,
    quint64 cacheGeneration)
{
    return _snapshots.acquire(session, epoch, [&] {
        auto base = load ? load() : std::shared_ptr<const Snapshot>{};
        return base && base->state.cacheGeneration == cacheGeneration ? base : nullptr;
    }, reused, cacheGeneration);
}

ObservationRead ObservationService::read(const ObservationReadRequest& request,
    const SessionDirectory::Entry& entry)
{
    QElapsedTimer captureTimer;
    captureTimer.start();
    ObservationRead result;
    result.captureId = request.captureId;

    bool reused = false;
    const auto generation = entry.session->core()->cacheGeneration();
    const auto base = snapshot(request.sessionId, request.epoch, [&] {
        // 每次真正向 Provider 取一份新基础摘要才计数；合并窗内复用不重复计。
        // 与 snapshotPublishCount（Parser 侧发布次数，≤4 Hz）是两个不同口径，
        // 二者比值即「客户端读取需求被合并/发布吸收了多少」。
        ++_coreCaptures;
        auto snapshot = entry.session->tryTerminalContext();
        if (NovaTerm::publishedContextSnapshotEnabled()) {
            const auto currentStats = entry.session->core()->publishedContextStatistics();
            const auto previousStats = _publishedStats.value(entry.id);
            _snapshotPublishes += currentStats.publishCount - previousStats.publishCount;
            _snapshotReuses += currentStats.reuseCount - previousStats.reuseCount;
            _publishedStats.insert(entry.id, currentStats);
        }
        return snapshot;
    }, &reused, generation);

    if (reused && request.freshness == FreshnessPolicy::StrictRevision) {
        // 仅旧 try-read 路径需要严格 revision 校验：合并窗内模型已推进就
        // 立即 Busy，不把陈旧结果当成新鲜捕获。发布路径按设计允许有界陈旧
        // （保留原 capturedAt），因此不做这项拒绝。
        const auto current = entry.session->core()->tryModelRevision();
        if (!base || !current || base->state.revision != *current) {
            result.status = ObservationRead::Status::Busy;
            return result;
        }
    }
    if (reused) {
        ++_reusedCaptures;
        ++_coalescedReads;
    }
    if (!base) {
        result.status = ObservationRead::Status::BusyShort;
        return result;
    }
    if (entry.session->core()->cacheGeneration() != generation) {
        result.status = ObservationRead::Status::Busy;
        return result;
    }

    const qint64 now = _clock ? _clock() : 0;
    const bool sourceTruncated = base->state.truncated;
    bool outputTruncated = false;
    bool reset = sourceTruncated
        || (request.sinceProvided
            && (request.sinceRevision < base->resetRevision
                || request.sinceRevision > base->state.revision
                || (base->cacheFloor > 0 && request.sinceRevision <= base->cacheFloor)));
    qsizetype remaining = request.maxBytes;
    int remainingLines = request.maxLines;
    const auto text = [&](const std::string& string) {
        const auto bytes = QByteArray::fromStdString(string);
        const auto clipped = clipUtf8(bytes, remaining);
        outputTruncated |= clipped.size() != bytes.size();
        remaining -= clipped.size();
        _projectionCopiedBytes += quint64(clipped.size());
        return QString::fromUtf8(clipped);
    };
    const auto line = [&](QVector<ObservationLine>& target, const std::string& value) {
        if (remainingLines == 0 || remaining == 0) {
            outputTruncated = true;
            return;
        }
        target.append(ObservationLine{text(value)});
        --remainingLines;
    };

    result.title = text(base->state.title);
    if (request.includeViewport) {
        for (const auto& value : base->state.viewport)
            line(result.viewport, value.text);
    }
    if (!base->state.alternateScreen) {
        for (const auto& value : base->entries) {
            if (!request.sinceProvided || reset || value.revision > request.sinceRevision)
                line(result.recent, value.text);
        }
    }
    reset |= outputTruncated;

    CaptureRecord record;
    record.scope = CaptureScope::Connection;
    record.owner = request.owner;
    record.session = request.sessionId;
    record.epoch = request.epoch;
    record.revision = QString::number(base->state.revision);
    record.cacheGeneration = generation;
    record.sourceTruncated = sourceTruncated;
    record.outputTruncated = outputTruncated;
    record.expires = now + Budget::CaptureLifetime.count();
    record.created = now;
    record.lines.reserve(result.viewport.size() + result.recent.size());
    for (const auto& value : result.viewport)
        record.lines.append(CaptureLine{value.text, QStringLiteral("viewport"), 0});
    for (const auto& value : result.recent)
        record.lines.append(CaptureLine{value.text, QStringLiteral("recent_output"), 0});
    if (entry.session->core()->cacheGeneration() != generation) {
        result = {};
        result.status = ObservationRead::Status::Busy;
        return result;
    }
    _captures.put(request.captureId, std::move(record));

    const auto capturedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        base->capturedAt.time_since_epoch()).count();
    const qint64 wallNow = _wallClock ? _wallClock()
        : QDateTime::currentMSecsSinceEpoch();
    result.snapshotAgeMs = (std::max<qint64>)(0, wallNow - capturedMs);
    if (_snapshotAges.size() == 256)
        _snapshotAges.pop_front();
    _snapshotAges.push_back(result.snapshotAgeMs);

    result.status = ObservationRead::Status::Ok;
    result.cacheGeneration = generation;
    result.revision = QString::number(base->state.revision);
    result.capturedAt = QDateTime::fromMSecsSinceEpoch(
        capturedMs, QTimeZone::UTC).toString(Qt::ISODateWithMs);
    result.sourceTruncated = sourceTruncated;
    result.outputTruncated = outputTruncated;
    result.resetRequired = reset;
    result.alternateScreen = base->state.alternateScreen;
    result.cursorRow = int(base->state.cursor.position.row);
    result.cursorColumn = int(base->state.cursor.position.col);
    result.suppressedDuplicates = base->suppressedDuplicates;
    if (_captureDurations.size() == 256)
        _captureDurations.pop_front();
    _captureDurations.push_back(captureTimer.nsecsElapsed());
    if (generation != entry.session->core()->cacheGeneration()) {
        _captures.remove(request.captureId);
        result = {};
        result.status = ObservationRead::Status::Busy;
    }
    return result;
}

CaptureSearchResult ObservationService::search(const CaptureRecord& capture,
    const QString& query, bool caseSensitive, const QString& scope, int limit,
    const std::function<bool()>& stop) const
{
    return searchCapture(capture, query, caseSensitive, scope, limit, stop);
}

void ObservationService::removeSession(const QString& session)
{
    _snapshots.remove(session);
    _captures.removeSession(session);
    _publishedStats.remove(session);
}

} // namespace NovaTerm::Mcp
