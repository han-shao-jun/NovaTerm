/**
 * @file ObservationService.h
 * @brief 统一观察服务：基础采样、读取投影与 capture 存储的唯一所有者。
 *
 * 兼容连接作用域与客户端作用域共用物理存储但分别计额；读取用例在此完成
 * 新鲜度校验、预算裁剪与 capture 记录，调用方只做 token 签发与 JSON 编码。
 */
#pragma once

#include "application/CaptureSearch.h"
#include "application/CaptureStore.h"
#include "application/SnapshotStore.h"
#include "mcp/contracts/Budget.h"
#include "session/SessionDirectory.h"
#include "session/TerminalContextProvider.h"

#include <QString>
#include <QVector>

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>

namespace NovaTerm::Mcp {

/** @brief 读取投影的一行文本；预算裁剪在行级或 UTF-8 边界上发生。 */
struct ObservationLine
{
    QString text;
};

/** @brief 读取的新鲜度策略；发布快照允许有界陈旧，try-read 路径严格校验。 */
enum class FreshnessPolicy
{
    BoundedStaleness,
    StrictRevision,
};

/** @brief 兼容读取用例的请求；绑定与预算都是显式值。 */
struct ObservationReadRequest
{
    QString sessionId;
    QString epoch;
    QString owner; ///< 连接作用域的 owner（连接 ID）。
    QString captureId;
    bool includeViewport{true};
    bool sinceProvided{false};
    quint64 sinceRevision{0};
    FreshnessPolicy freshness{FreshnessPolicy::BoundedStaleness};
    qsizetype maxBytes{Budget::ObservationDefaultPayloadBytes};
    int maxLines{Budget::ObservationDefaultLines};
};

/** @brief 读取结果；调用方只做协议 token 与 JSON 封装。 */
struct ObservationRead
{
    enum class Status
    {
        Ok,
        Busy,      ///< 合并窗外采样失败或严格新鲜度不符，可短暂重试。
        BusyShort, ///< 尚无可用基础摘要。
    };

    Status status{Status::BusyShort};
    QString captureId;
    QString revision;
    QString capturedAt;
    bool sourceTruncated{false};
    bool outputTruncated{false};
    bool resetRequired{false};
    bool alternateScreen{false};
    bool historyMayPrecedeEpoch{true};
    QString title;
    int cursorRow{0};
    int cursorColumn{0};
    quint64 suppressedDuplicates{0};
    QVector<ObservationLine> viewport;
    QVector<ObservationLine> recent;
    qint64 snapshotAgeMs{0};
};

class ObservationService
{
public:
    using Snapshot = TerminalContextProvider::Snapshot;
    using Loader = SnapshotStore<Snapshot>::Loader;

    explicit ObservationService(std::function<qint64()> clock,
        std::function<qint64()> wallClock = {});

    /** @brief 合并窗口内复用同一基础摘要。 */
    [[nodiscard]] std::shared_ptr<const Snapshot> snapshot(const QString& session,
        const QString& epoch, const Loader& load, bool* reused = nullptr);

    /** @brief 兼容 read_context 的读取/投影/capture 用例。 */
    [[nodiscard]] ObservationRead read(const ObservationReadRequest& request,
        const SessionDirectory::Entry& entry);

    /**
     * @brief 在已授权的有界 capture 上搜索。
     *
     * 校验与中断语义由调用方处理；匹配算法见 `application/CaptureSearch`。
     */
    [[nodiscard]] CaptureSearchResult search(const CaptureRecord& capture,
        const QString& query, bool caseSensitive, const QString& scope, int limit,
        const std::function<bool()>& stop = {}) const;

    [[nodiscard]] CaptureStore& captures() noexcept { return _captures; }
    [[nodiscard]] const CaptureStore& captures() const noexcept { return _captures; }
    void removeSession(const QString& session);
    [[nodiscard]] std::size_t captureCount() const { return _captures.size(); }

    // 统计口径：合并窗内外、Parser 发布与投影字节数。
    [[nodiscard]] quint64 reusedCaptureCount() const { return _reusedCaptures; }
    [[nodiscard]] quint64 coreCaptureCount() const { return _coreCaptures; }
    [[nodiscard]] quint64 snapshotPublishCount() const { return _snapshotPublishes; }
    [[nodiscard]] quint64 snapshotReuseCount() const { return _snapshotReuses; }
    [[nodiscard]] quint64 coalescedReadCount() const { return _coalescedReads; }
    [[nodiscard]] quint64 projectionCopiedBytes() const { return _projectionCopiedBytes; }
    [[nodiscard]] const std::deque<qint64>& captureDurations() const
    {
        return _captureDurations;
    }
    [[nodiscard]] const std::deque<qint64>& snapshotAges() const { return _snapshotAges; }

private:
    SnapshotStore<Snapshot> _snapshots;
    CaptureStore _captures;
    std::function<qint64()> _clock;
    std::function<qint64()> _wallClock;
    QHash<QString, NovaTerm::PublishedContextStatistics> _publishedStats;

    quint64 _reusedCaptures{0};
    quint64 _coreCaptures{0};
    quint64 _snapshotPublishes{0};
    quint64 _snapshotReuses{0};
    quint64 _coalescedReads{0};
    quint64 _projectionCopiedBytes{0};
    std::deque<qint64> _captureDurations;
    std::deque<qint64> _snapshotAges;
};

} // namespace NovaTerm::Mcp
