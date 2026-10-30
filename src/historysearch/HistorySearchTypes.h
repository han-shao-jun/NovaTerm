#pragma once
#include "recording/HistoryTypes.h"
#include <QStringList>
#include <atomic>
#include <memory>

namespace NovaTerm::Recording {
// Constructed by the authenticated application adapter, never decoded from tool arguments.
struct ResourceContext { QString principalId; quint64 tokenEpoch{0}; };
struct ResourceGrant {
    QString principalId;
    quint64 tokenEpoch{0};
    HistoryCaptureId captureId;
    quint64 resourceGeneration{1};
    quint64 accessVersion{0};
    quint64 firstRecordId{1};
    quint64 lastRecordId{0}; // Zero means the committed high water at request admission.
    std::optional<qint64> firstTimestampMs;
    std::optional<qint64> lastTimestampMs;
    bool persistent{false};
};
struct HistoryResource { CaptureDescriptor capture; quint64 generation{1}; };
struct HistoryReadPort {
    // Owner-thread boundary. Storage adapters deliver completions on the same
    // application thread; false means no operation was queued.
    std::function<std::optional<HistoryResource>(HistoryCaptureId)> describe;
    std::function<bool(HistoryCaptureId, HistoryRange, std::function<void(ReadResult)>)> readCommitted;
    std::function<std::vector<HistoryResource>()> catalog{};
};
struct HistoryRawRange {
    quint64 recordId{0};
    qsizetype byteOffset{0};
    qsizetype byteCount{0};
};
struct HistoryReadRequest {
    HistoryCaptureId captureId;
    QString cursor;
    qsizetype maxResponseBytes{64 * 1024};
    qsizetype maxPayloadBytes{64 * 1024};
    std::shared_ptr<std::atomic<bool>> cancelled;
    // Owner-thread admission predicate; never evaluated by a CPU/storage worker.
    std::function<bool()> admitted;
};
struct HistoryReadFragment {
    HistoryRecord record;
    qsizetype byteOffset{0}; // Payload is this fragment, not the complete record.
};
struct HistoryReadPage {
    std::vector<HistoryReadFragment> records;
    quint64 highWaterRecordId{0};
    qsizetype byteCount{0};
    Integrity sourceCompleteness{Integrity::UnknownTail};
    bool truncated{false};
    QString nextCursor;
    QString error;
};
enum class HistorySearchMode { RawBytes, Text };
struct HistorySearchRequest {
    HistoryCaptureId captureId;
    QByteArray query; // Literal raw bytes or explicit UTF-8, according to mode.
    HistorySearchMode mode{HistorySearchMode::Text};
    QString cursor;
    int maxMatches{128};
    qsizetype maxResponseBytes{64 * 1024};
    qsizetype scanBytes{16 * 1024 * 1024};
    int scanMilliseconds{2000};
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::function<bool()> admitted;
};
struct HistorySearchHit { std::vector<HistoryRawRange> ranges; };
struct HistorySearchPage {
    std::vector<HistorySearchHit> hits;
    std::vector<HistoryRawRange> projectionGaps;
    quint64 highWaterRecordId{0};
    Integrity sourceCompleteness{Integrity::UnknownTail};
    QString projectionVersion{QStringLiteral("utf8-lines-cr-ansi-v1")};
    bool complete{false};
    QString nextCursor;
    QString reason;
    QString error;
};
struct HistorySearchLimits {
    int idleMilliseconds{60000};
    int lifetimeMilliseconds{300000};
    size_t maxStates{16};
    size_t maxStatesPerPrincipal{2};
};
}
