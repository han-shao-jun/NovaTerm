#pragma once
#include <QByteArray>
#include <QString>
#include <QUuid>
#include <optional>
#include <vector>
#include <functional>

// Persistent receive-stream recording. Deliberately separate from terminal scrollback.
namespace NovaTerm::Recording {
struct HistoryCaptureId {
    QUuid value;
    QString text() const { return value.toString(QUuid::WithoutBraces); }
    bool valid() const { return !value.isNull(); }
    friend bool operator==(const HistoryCaptureId& a, const HistoryCaptureId& b) { return a.value == b.value; }
    friend bool operator!=(const HistoryCaptureId& a, const HistoryCaptureId& b) { return !(a == b); }
};
enum class RecordKind : quint8 { Payload, Gap, Boundary };
enum class SubmitResult { Accepted, GapRecorded, Disabled };
enum class Integrity { Lossless, KnownGap, UnknownTail, RecoveredUnsealed };
struct HistoryRecord {
    HistoryCaptureId captureId;
    QUuid sessionId;
    quint64 connectionGeneration{0};
    quint64 recordId{0};
    quint64 lastRecordId{0}; // Inclusive; gaps may coalesce successive dropped submissions.
    qint64 monotonicTimestampNs{0};
    qint64 wallTimestampMs{0};
    RecordKind kind{RecordKind::Payload};
    QByteArray payload;
    QString reason;
    std::optional<quint64> lostRecordCount;
    std::optional<quint64> lostByteCount;
};
struct CaptureDescriptor {
    HistoryCaptureId id;
    QUuid sessionId;
    QUuid bootId;
    quint64 connectionGeneration{0};
    qint64 createdMs{0};
    quint64 highWaterRecordId{0};
    quint64 bytes{0};
    quint64 gapBytes{0};
    Integrity integrity{Integrity::UnknownTail};
    bool active{false};
    bool retained{false};
    QString stopReason;
};
struct HistoryRange {
    quint64 firstRecordId{1};
    quint64 lastRecordId{0}; // Zero means current high-water.
    qsizetype maxBytes{64 * 1024}; // Raw payload budget; metadata separately bounded by maxRecords.
    size_t maxRecords{256};
};
struct ReadResult {
    std::vector<HistoryRecord> records;
    quint64 highWaterRecordId{0};
    quint64 nextRecordId{0};
    Integrity integrity{Integrity::UnknownTail};
    std::optional<quint64> lostRecordCount;
    std::optional<quint64> lostByteCount;
    bool missing{false};
    QString error;
};
struct StorageOptions {
    qsizetype chunkBytes{4 * 1024 * 1024};
    quint64 maxCaptureBytes{512ull * 1024 * 1024};
};
enum class IoPoint { DataWrite, DataSync, ManifestReplace, IndexCommit, Seal, RecoveryScan, ReadAccess, DirectoryScan };
using IoProbe = std::function<bool(IoPoint)>;
struct RecordingIntent { QUuid sessionId; quint64 connectionGeneration{0}; };
struct RecordingResult { bool success{false}; HistoryCaptureId captureId; QString error; };
}
