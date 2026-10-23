#pragma once
#include "HistoryAuthorizationStore.h"
#include <atomic>
#include <memory>
#include <chrono>

namespace NovaTerm::Recording {
struct RawSpan {
    quint64 record{0};
    quint32 offset{0}, length{0};
};
struct HistoryReadRequest {
    HistoryCaptureId capture;
    QString cursor;
    int maxBytes{24 * 1024};
};
struct ReadFragment {
    HistoryRecord record;
    quint32 offset{0};
};
struct HistoryReadPage {
    std::vector<ReadFragment> records;
    QString cursor, error;
    quint64 highWater{0}, byteCount{0};
    Integrity integrity{Integrity::UnknownTail};
    bool truncated{false};
};
enum class SearchMode { RawBytes, Text };
struct HistorySearchRequest {
    HistoryCaptureId capture;
    SearchMode mode{SearchMode::Text};
    QByteArray query;
    QString cursor;
    int maxMatches{32};
};
struct HistoryMatch { std::vector<RawSpan> spans; QString evidenceId; };
struct HistorySearchPage {
    std::vector<HistoryMatch> matches;
    std::vector<RawSpan> projectionGap;
    QString cursor, error, reason;
    quint64 highWater{0};
    Integrity integrity{Integrity::UnknownTail};
    bool complete{false};
    int projectionVersion{1};
};
using Cancellation = std::shared_ptr<std::atomic<bool>>;
// Application-thread entry. Disk work remains owned by the P2 worker.
class HistorySourceReader {
public:
    virtual ~HistorySourceReader() = default;
    virtual std::vector<CaptureDescriptor> captures() const = 0;
    virtual bool read(HistoryCaptureId, HistoryRange, std::function<void(ReadResult)>) = 0;
};
}
