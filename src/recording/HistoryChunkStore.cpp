#include "HistoryChunkStore.h"
#include "HistoryIndex.h"
#include "HistoryStorageIo.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

namespace NovaTerm::Recording {
namespace {
constexpr qsizetype MaxFrame = 128 * 1024;
QByteArray hash(const QByteArray& data) { return QCryptographicHash::hash(data, QCryptographicHash::Sha256); }
QString chunkName(int n) { return QStringLiteral("%1.bin").arg(n, 8, 10, QLatin1Char('0')); }
QByteArray encode(const HistoryRecord& r) {
    QByteArray b; QDataStream s(&b, QIODevice::WriteOnly); s.setVersion(QDataStream::Qt_6_8);
    s << quint8(r.kind) << r.captureId.value << r.sessionId << r.connectionGeneration
      << r.recordId << r.lastRecordId << r.monotonicTimestampNs << r.wallTimestampMs
      << r.reason << bool(r.lostRecordCount) << r.lostRecordCount.value_or(0)
      << bool(r.lostByteCount) << r.lostByteCount.value_or(0) << r.payload;
    return b;
}
bool decode(const QByteArray& b, HistoryRecord& r) {
    QDataStream s(b); s.setVersion(QDataStream::Qt_6_8); quint8 kind; bool records, bytes; quint64 nr, nb;
    s >> kind >> r.captureId.value >> r.sessionId >> r.connectionGeneration
      >> r.recordId >> r.lastRecordId >> r.monotonicTimestampNs >> r.wallTimestampMs
      >> r.reason >> records >> nr >> bytes >> nb >> r.payload;
    if (s.status() != QDataStream::Ok || !s.atEnd() || kind > quint8(RecordKind::Boundary)
        || r.recordId == 0 || r.lastRecordId < r.recordId || r.payload.size() > 64 * 1024
        || r.reason.size() > 128) return false;
    r.kind = RecordKind(kind); if (records) r.lostRecordCount = nr; if (bytes) r.lostByteCount = nb;
    return r.kind == RecordKind::Payload || r.payload.isEmpty();
}
bool readFrame(QFile& f, qint64 end, HistoryRecord& r, QByteArray& digest) {
    if (end - f.pos() < 12) return false;
    const auto header = f.read(12); if (header.size() != 12 || header.left(8) != "NTHREC01") return false;
    QDataStream stream(header.mid(8)); quint32 length = 0; stream >> length;
    if (!length || length > MaxFrame || qint64(length) + 32 > end - f.pos()) return false;
    const auto body = f.read(length); digest = f.read(32);
    return body.size() == length && digest == hash(body) && decode(body, r);
}
QByteArray manifestBytes(QJsonObject object) {
    object["checksum"] = QString::fromLatin1(hash(QJsonDocument(object).toJson(QJsonDocument::Compact)).toHex());
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
bool manifestValid(QJsonObject object) {
    const auto checksum = object.take("checksum").toString().toLatin1();
    return checksum == hash(QJsonDocument(object).toJson(QJsonDocument::Compact)).toHex();
}
}
struct HistoryChunkStore::Impl {
    QString root, directory, error;
    CaptureDescriptor capture;
    StorageOptions options;
    FaultProbe probe;
    StorageIo::DirectoryGuard guard, chunksGuard;
    QFile chunk;
    HistoryIndex index;
    struct Position { HistoryRecord record; int chunk; qint64 offset; QByteArray digest; };
    std::vector<Position> pending;
    std::vector<qint64> sizes;
    std::vector<qint64> committedSizes;
    QByteArray chain{32, '\0'};
    QByteArray committedChain{32, '\0'};
    quint64 written{0}, committed{0}, gapBytes{0}, gapRecords{0};
    quint64 committedGapBytes{0}, committedGapRecords{0};
    quint64 totalBytes{0};
    bool writable{false}, failed{false}, sealed{false}, hadGap{false}, committedHadGap{false}, unknownGap{false};
    Integrity integrity{Integrity::UnknownTail};
    bool fail(const QString& why) { error = why; failed = true; writable = false; return false; }
    bool allow(IoPoint point) { return !probe || probe(point); }
    QJsonObject manifest(bool seal, const QString& reason = {}) const {
        QJsonArray chunks; for (const auto size : sizes) chunks.append(QString::number(size));
        return {{"version", 1}, {"id", capture.id.text()}, {"session", capture.sessionId.toString(QUuid::WithoutBraces)},
            {"boot", capture.bootId.toString(QUuid::WithoutBraces)}, {"generation", QString::number(capture.connectionGeneration)},
            {"created_ms", QString::number(capture.createdMs)}, {"high_water", QString::number(written)},
            {"chain", QString::fromLatin1(chain.toHex())}, {"chunks", chunks}, {"sealed", seal}, {"reason", reason},
            {"gap_bytes", QString::number(gapBytes)}, {"gap_records", QString::number(gapRecords)}, {"had_gap", hadGap}};
    }
    bool publish(bool seal, const QString& reason = {}) {
        // The private directory is already validated and pinned. Re-walking all
        // historical chunks on every commit is unnecessary; validate the actual
        // publication path, and revalidate individual chunks when they are read.
        if (!StorageIo::safePath(directory + "/manifest.json") || !allow(IoPoint::ManifestReplace)
            || !StorageIo::replaceDurable(directory + "/manifest.json", manifestBytes(manifest(seal, reason))))
            return fail("manifest publication failed: " + StorageIo::error());
        return true;
    }
};
HistoryChunkStore::HistoryChunkStore(QString root, CaptureDescriptor d, StorageOptions o, FaultProbe probe)
    : _impl(std::make_unique<Impl>()) {
    auto& p = *_impl; p.root = std::move(root); p.capture = std::move(d); p.options = o;
    p.options.chunkBytes = std::clamp<qsizetype>(o.chunkBytes, 1024, 4 * 1024 * 1024);
    p.directory = p.root + "/" + p.capture.id.text(); p.probe = std::move(probe);
}
HistoryChunkStore::~HistoryChunkStore() = default;
bool HistoryChunkStore::open() {
    auto& p = *_impl;
    if (!p.capture.id.valid() || QFileInfo::exists(p.directory)) return p.fail("capture already exists or invalid id");
    if (!StorageIo::privateDirectory(p.root) || !StorageIo::privateDirectory(p.directory)
        || !p.guard.lock(p.directory) || !StorageIo::privateDirectory(p.directory + "/chunks")
        || !p.chunksGuard.lock(p.directory + "/chunks")) return p.fail("unsafe history directory");
    p.capture.active = true; p.writable = true;
    // Durable open marker precedes acceptance of any receive bytes.
    if (!p.publish(false) || !StorageIo::syncDirectory(p.root) || !p.index.open(p.directory)) return p.fail("capture initialization failed");
    return true;
}
bool HistoryChunkStore::append(const std::vector<HistoryRecord>& records) {
    auto& p = *_impl; if (!p.writable || p.failed || p.sealed) return false;
    if (records.size() > 4096 || p.pending.size() + records.size() > 4096) return p.fail("uncommitted metadata limit");
    for (auto r : records) {
        if (r.recordId != p.written + 1 || r.payload.size() > 64 * 1024 || r.reason.size() > 128)
            return p.fail("invalid record order or size");
        r.captureId = p.capture.id; r.sessionId = p.capture.sessionId; r.connectionGeneration = p.capture.connectionGeneration;
        if (!r.lastRecordId) r.lastRecordId = r.recordId;
        if (r.lastRecordId < r.recordId || (r.kind != RecordKind::Payload && !r.payload.isEmpty())) return p.fail("invalid record");
        const auto body = encode(r); const auto digest = hash(body);
        QByteArray frame("NTHREC01"); QDataStream stream(&frame, QIODevice::Append); stream << quint32(body.size());
        frame += body; frame += digest;
        if (quint64(frame.size()) > p.options.maxCaptureBytes - (std::min)(p.totalBytes, p.options.maxCaptureBytes))
            return p.fail("capture_quota");
        if (!p.chunk.isOpen() || (p.chunk.size() > 0 && p.chunk.size() + frame.size() > p.options.chunkBytes)) {
            if (p.chunk.isOpen() && (!p.allow(IoPoint::DataSync) || !StorageIo::sync(p.chunk))) return p.fail("chunk sync failed");
            p.chunk.close();
            if (p.sizes.size() >= 8192 || !StorageIo::safeTree(p.directory)) return p.fail("unsafe chunk path or chunk count limit");
            p.chunk.setFileName(p.directory + "/chunks/" + chunkName(int(p.sizes.size()) + 1));
            if (!p.chunk.open(QIODevice::WriteOnly | QIODevice::NewOnly) || !StorageIo::privateFile(p.chunk.fileName())) return p.fail("chunk creation failed");
            p.sizes.push_back(0);
        }
        const auto offset = p.chunk.pos();
        if (!p.allow(IoPoint::DataWrite) || p.chunk.write(frame) != frame.size()) return p.fail("chunk write failed");
        p.pending.push_back({r, int(p.sizes.size()), offset, digest});
        p.sizes.back() += frame.size(); p.totalBytes += quint64(frame.size());
        p.chain = hash(p.chain + digest); p.written = r.lastRecordId;
        if (r.kind == RecordKind::Gap) {
            if (!r.lostByteCount || !r.lostRecordCount) p.unknownGap = true;
            p.hadGap = true; p.gapBytes += r.lostByteCount.value_or(0); p.gapRecords += r.lostRecordCount.value_or(0);
        }
    }
    return true;
}
bool HistoryChunkStore::commit() {
    auto& p = *_impl; if (!p.writable || p.failed) return false;
    if (p.chunk.isOpen() && (!p.allow(IoPoint::DataSync) || !StorageIo::sync(p.chunk))) return p.fail("chunk sync failed");
    if (!StorageIo::syncDirectory(p.directory + "/chunks") || !p.publish(false)) return false;
    // Index is strictly after the manifest; failure leaves an unsealed recoverable prefix.
    if (!p.allow(IoPoint::IndexCommit) || !p.index.begin()) return p.fail("index transaction failed");
    for (const auto& location : p.pending)
        if (!p.index.add(location.record, location.chunk, location.offset, location.digest)) return p.fail("index update failed");
    if (!p.index.commit()) return p.fail("index commit failed");
    p.committed = p.written; p.committedSizes = p.sizes; p.committedChain = p.chain;
    p.committedGapBytes = p.gapBytes; p.committedGapRecords = p.gapRecords; p.committedHadGap = p.hadGap;
    p.pending.clear(); return true;
}
bool HistoryChunkStore::seal(const QString& reason) {
    auto& p = *_impl;
    if (!p.writable || p.failed || !p.pending.empty() || p.committed != p.written) return false;
    if (p.unknownGap) return p.fail("unknown_tail cannot be sealed as complete");
    if (!p.allow(IoPoint::Seal)) return p.fail("seal persistence failed");
    if (!p.publish(true, reason)) return false;
    p.sealed = true; p.writable = false; p.capture.active = false; p.capture.stopReason = reason;
    p.integrity = p.hadGap ? Integrity::KnownGap : Integrity::Lossless; return true;
}
bool HistoryChunkStore::recover() {
    auto& p = *_impl;
    if (!p.allow(IoPoint::RecoveryScan) || !p.capture.id.valid() || !QFileInfo(p.directory).isDir() || !StorageIo::secureTree(p.directory) || !p.guard.lock(p.directory)
        || !p.chunksGuard.lock(p.directory + "/chunks")) return p.fail("unsafe or missing capture");
    QFile mf(p.directory + "/manifest.json"); QJsonObject object;
    if (mf.open(QIODevice::ReadOnly) && mf.size() <= 256 * 1024) object = QJsonDocument::fromJson(mf.readAll()).object();
    const bool valid = manifestValid(object) && object["version"].toInt() == 1 && object["id"].toString() == p.capture.id.text()
        && object["chunks"].isArray() && object["chunks"].toArray().size() <= 8192;
    p.integrity = valid ? Integrity::UnknownTail : Integrity::RecoveredUnsealed;
    if (valid) {
        p.capture.sessionId = QUuid(object["session"].toString()); p.capture.bootId = QUuid(object["boot"].toString());
        p.capture.connectionGeneration = object["generation"].toString().toULongLong();
        p.capture.createdMs = object["created_ms"].toString().toLongLong(); p.capture.stopReason = object["reason"].toString();
    }
    // Recovery always rebuilds a derived index, including a valid SQLite file with
    // a corrupt/wrong schema. No index content is evidence of committed bytes.
    for (const auto& suffix : {"", "-wal", "-shm"}) {
        const auto path = p.directory + "/index.sqlite" + suffix;
        if (!StorageIo::removePrivateFile(path)) return p.fail("cannot replace derived index: " + StorageIo::error());
    }
    if (!p.index.open(p.directory) || !p.index.beginRebuild()) return p.fail("index recovery failed");
    const auto listed = QDir(p.directory + "/chunks").entryList({"*.bin"}, QDir::Files, QDir::Name);
    const auto chunks = object["chunks"].toArray();
    const int count = valid ? int(chunks.size()) : (std::min)(int(listed.size()), 8192);
    bool complete = true;
    bool extraTail = valid && listed.size() != count;
    for (int i = 0; i < count; ++i) {
        QFile f(p.directory + "/chunks/" + chunkName(i + 1));
        if (!f.open(QIODevice::ReadOnly)) { complete = false; break; }
        const qint64 expected = valid ? chunks[i].toString().toLongLong() : f.size();
        if (expected < 0 || expected > 4 * 1024 * 1024 + MaxFrame) { complete = false; break; }
        if (valid && expected != f.size()) extraTail = true;
        const qint64 readableEnd = (std::min)(expected, f.size());
        qint64 good = 0;
        while (f.pos() < readableEnd) {
            if (!p.allow(IoPoint::RecoveryScan)) return p.fail("recovery scan interrupted");
            HistoryRecord r; QByteArray digest; const auto offset = f.pos();
            if (!readFrame(f, readableEnd, r, digest) || r.captureId != p.capture.id || r.recordId != p.committed + 1
                || (valid && (r.sessionId != p.capture.sessionId || r.connectionGeneration != p.capture.connectionGeneration))) {
                complete = false; break;
            }
            if (!p.index.add(r, i + 1, offset, digest)) return p.fail("index rebuild failed");
            p.committed = r.lastRecordId; p.chain = hash(p.chain + digest); good = f.pos();
            if (r.kind == RecordKind::Gap) {
                p.hadGap = true; p.gapBytes += r.lostByteCount.value_or(0); p.gapRecords += r.lostRecordCount.value_or(0);
                if (!r.lostByteCount || !r.lostRecordCount) p.unknownGap = true;
            }
        }
        if (good != expected) complete = false;
        p.committedSizes.push_back(good); p.totalBytes += quint64(good);
        if (!complete) break;
    }
    if (!p.index.commit()) return p.fail("index rebuild commit failed");
    complete = complete && p.committed == object["high_water"].toString().toULongLong()
        && p.chain.toHex() == object["chain"].toString().toLatin1()
        && p.gapBytes == object["gap_bytes"].toString().toULongLong()
        && p.gapRecords == object["gap_records"].toString().toULongLong()
        && p.hadGap == object["had_gap"].toBool();
    if (valid && complete && !extraTail && !p.unknownGap && object["sealed"].toBool()) {
        p.sealed = true; p.integrity = p.hadGap ? Integrity::KnownGap : Integrity::Lossless;
    }
    p.committedGapBytes = p.gapBytes; p.committedGapRecords = p.gapRecords; p.committedHadGap = p.hadGap;
    p.capture.active = false; p.writable = false; return true;
}
ReadResult HistoryChunkStore::readCommitted(HistoryRange range) {
    auto& p = *_impl; ReadResult result; result.highWaterRecordId = p.committed; result.integrity = p.integrity;
    if (p.sealed) { result.lostByteCount = p.committedGapBytes; result.lostRecordCount = p.committedGapRecords; }
    const auto missing = [&result](const QString& message) {
        result.missing = true; result.error = message; result.integrity = Integrity::UnknownTail;
        result.lostByteCount.reset(); result.lostRecordCount.reset(); result.nextRecordId = 0;
    };
    // Pinned private parents are already validated. Only inspect accessed paths;
    // walking every historical chunk would make every small page unbounded.
    if (!p.allow(IoPoint::ReadAccess) || !QFileInfo(p.directory).isDir() || !StorageIo::safePath(p.directory)) {
        missing("capture missing or unsafe"); return result;
    }
    if (!range.firstRecordId) range.firstRecordId = 1;
    const auto last = range.lastRecordId ? (std::min)(range.lastRecordId, p.committed) : p.committed;
    if (range.firstRecordId > last) return result;
    const auto limit = std::clamp<size_t>(range.maxRecords, 1, 1024);
    const auto budget = std::clamp<qsizetype>(range.maxBytes, 1, 256 * 1024);
    qsizetype used = 0; quint64 expected = range.firstRecordId; bool limited = false;
    const auto locations = p.index.locate(range.firstRecordId, last, limit);
    for (const auto& location : locations) {
        if (location.chunk < 1 || size_t(location.chunk) > p.committedSizes.size()) { result.missing = true; break; }
        const auto path = p.directory + "/chunks/" + chunkName(location.chunk);
        QFile f(path);
        HistoryRecord r; QByteArray digest;
        if (!p.allow(IoPoint::ReadAccess) || !StorageIo::safeFile(path) || !f.open(QIODevice::ReadOnly) || !f.seek(location.offset)
            || !readFrame(f, p.committedSizes[size_t(location.chunk - 1)], r, digest)
            || r.captureId != p.capture.id || r.recordId != location.first || r.lastRecordId != location.last) {
            result.missing = true; break;
        }
        if (r.recordId > expected || r.lastRecordId < expected
            || (!result.records.empty() && r.recordId != expected)
            || (r.recordId < expected && r.kind != RecordKind::Gap)) { result.missing = true; break; }
        const auto charge = r.payload.size();
        if (charge > budget - used && !result.records.empty()) { result.nextRecordId = r.recordId; limited = true; break; }
        // A caller must provide enough budget for a whole record; do not silently skip it.
        if (charge > budget - used) { result.nextRecordId = r.recordId; result.error = "record exceeds read budget"; limited = true; break; }
        expected = r.lastRecordId + 1; used += charge; result.records.push_back(std::move(r));
    }
    if (!limited && locations.size() < limit && expected <= last) result.missing = true;
    if (!result.nextRecordId && !result.records.empty() && result.records.back().lastRecordId < last)
        result.nextRecordId = result.records.back().lastRecordId + 1;
    if (range.firstRecordId <= last && result.records.empty() && result.error.isEmpty()) result.missing = true;
    if (result.missing) missing("committed range missing or corrupt");
    return result;
}
void HistoryChunkStore::setCaptureLimit(quint64 bytes) { _impl->options.maxCaptureBytes = bytes; }
CaptureDescriptor HistoryChunkStore::descriptor() const {
    auto result = _impl->capture; result.highWaterRecordId = _impl->committed;
    result.bytes = _impl->totalBytes; result.gapBytes = _impl->committedGapBytes; result.integrity = _impl->integrity; return result;
}
QString HistoryChunkStore::error() const { return _impl->error; }
QString HistoryChunkStore::directory() const { return _impl->directory; }
}
