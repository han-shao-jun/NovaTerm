#include "historysearch/HistoryReadService.h"
#include "historysearch/HistorySearchService.h"
#include "historysearch/HistoryProjectionDecoder.h"
#include <QtTest>
#include <QEventLoop>
#include <QTimer>
#include <algorithm>
using namespace NovaTerm::Recording;
namespace {
struct Fixture {
    HistoryCaptureId id{QUuid::createUuid()};
    std::vector<HistoryRecord> records;
    quint64 generation{1};
    bool exists{true};
    HistoryAuthorizationStore authorization;
    ResourceContext context{"principal", 7};
    void add(QByteArray bytes, RecordKind kind = RecordKind::Payload) {
        HistoryRecord r; r.captureId = id; r.recordId = records.size() + 1; r.lastRecordId = r.recordId;
        r.kind = kind; r.payload = bytes; r.wallTimestampMs = 100; records.push_back(r);
    }
    ResourceGrant grant() {
        ResourceGrant g; g.principalId = context.principalId; g.tokenEpoch = context.tokenEpoch;
        g.captureId = id; g.resourceGeneration = generation; return authorization.setGrant(g);
    }
    HistoryReadPort port() {
        return {[this](HistoryCaptureId capture) -> std::optional<HistoryResource> {
            if (!exists || capture != id) return {};
            CaptureDescriptor c; c.id = id; c.highWaterRecordId = records.size(); c.integrity = Integrity::Lossless;
            return HistoryResource{c, generation};
        }, [this](HistoryCaptureId, HistoryRange range, auto callback) {
            ReadResult r; r.highWaterRecordId = records.size(); r.integrity = Integrity::Lossless;
            qsizetype bytes = 0;
            for (const auto& record : records) {
                if (record.recordId < range.firstRecordId || record.recordId > range.lastRecordId) continue;
                if (r.records.size() >= range.maxRecords || bytes + record.payload.size() > range.maxBytes) {
                    r.nextRecordId = record.recordId; break;
                }
                bytes += record.payload.size(); r.records.push_back(record);
            }
            callback(std::move(r)); return true;
        }};
    }
};
HistorySearchPage search(HistorySearchService& service, HistorySearchRequest request, ResourceContext context) {
    QEventLoop loop; std::optional<HistorySearchPage> page;
    service.searchAsync(request, context, [&](auto result) { page = std::move(result); loop.quit(); });
    if (!page) { QTimer::singleShot(5000, &loop, &QEventLoop::quit); loop.exec(); }
    if (!page) { HistorySearchPage error; error.error = "TEST_TIMEOUT"; return error; }
    return std::move(*page);
}
HistoryReadPage read(HistoryReadService& service, HistoryReadRequest request, ResourceContext context) {
    QEventLoop loop; std::optional<HistoryReadPage> page;
    service.readAsync(request, context, [&](auto result) { page = std::move(result); loop.quit(); });
    if (!page) { QTimer::singleShot(5000, &loop, &QEventLoop::quit); loop.exec(); }
    if (!page) { HistoryReadPage error; error.error = "TEST_TIMEOUT"; return error; }
    return std::move(*page);
}
}
class HistorySearchTests : public QObject {
    Q_OBJECT
private slots:
    void cancelledReadsDoNotRetainUnreachableCursors() {
        Fixture f; f.add(QByteArray(65536, 'x')); f.grant(); HistoryReadService service(f.port(), f.authorization);
        HistoryReadRequest request; request.captureId = f.id; request.maxPayloadBytes = 1;
        for (int i = 0; i < 70; ++i) {
            auto cancelled = std::make_shared<std::atomic<bool>>(false);
            request.cancelled = cancelled; std::optional<HistoryReadPage> page;
            service.readAsync(request, f.context, [&](auto result) { page = std::move(result); });
            cancelled->store(true); QTRY_VERIFY(page.has_value());
            QCOMPARE(page->error, QString("CANCELLED")); QVERIFY(page->records.empty()); QVERIFY(page->nextCursor.isEmpty());
        }
        ResourceGrant other = f.grant(); other.principalId = "other"; f.authorization.setGrant(other);
        request.cancelled.reset(); QVERIFY(read(service, request, {"other", 7}).error.isEmpty());
    }
    void preCancelledReadDoesNotCallStorage() {
        Fixture f; f.add("log"); f.grant(); int calls = 0; auto port = f.port(); const auto original = port.readCommitted;
        port.readCommitted = [&](auto id, auto range, auto done) { ++calls; return original(id, range, std::move(done)); };
        HistoryReadService service(port, f.authorization); HistoryReadRequest request; request.captureId = f.id;
        request.cancelled = std::make_shared<std::atomic<bool>>(true);
        QCOMPARE(read(service, request, f.context).error, QString("CANCELLED")); QCOMPARE(calls, 0);
    }
    void completedInitialSearchesReleaseStateSlots() {
        Fixture f; f.add("hello\n"); f.grant(); HistorySearchService service(f.port(), f.authorization);
        HistorySearchRequest q; q.captureId = f.id; q.query = "hello";
        for (int i = 0; i < 4; ++i) {
            const auto page = search(service, q, f.context);
            QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
            QVERIFY(page.complete); QVERIFY(page.nextCursor.isEmpty()); QCOMPARE(page.hits.size(), size_t(1));
        }
    }
    void searchRetryBindsResponseLimits() {
        Fixture f; f.add(QByteArray("ab ").repeated(60)); f.grant(); HistorySearchService service(f.port(), f.authorization);
        HistorySearchRequest q; q.captureId = f.id; q.query = "ab"; q.mode = HistorySearchMode::RawBytes; q.maxMatches = 16;
        q.cursor = search(service, q, f.context).nextCursor; QVERIFY(!q.cursor.isEmpty());
        const auto page = search(service, q, f.context); QCOMPARE(page.hits.size(), size_t(16));
        auto changed = q; changed.maxMatches = 1;
        QCOMPARE(search(service, changed, f.context).error, QString("CURSOR_MISMATCH"));
        changed = q; changed.maxResponseBytes = 2048;
        QCOMPARE(search(service, changed, f.context).error, QString("CURSOR_MISMATCH"));
        const auto retry = search(service, q, f.context);
        QCOMPARE(retry.hits.size(), page.hits.size()); QCOMPARE(retry.nextCursor, page.nextCursor);
        q.cursor = page.nextCursor; q.maxMatches = 1;
        const auto next = search(service, q, f.context); QVERIFY(next.error.isEmpty()); QCOMPARE(next.hits.size(), size_t(1));
    }
    void ansiIntermediateAndControlStringsAreStripped_data() {
        QTest::addColumn<QByteArray>("control");
        QTest::newRow("charset") << QByteArray("\x1b(B");
        QTest::newRow("multiple-intermediates") << QByteArray("\x1b(#B");
        QTest::newRow("dcs") << QByteArray("\x1bPhidden\x1b\\");
        QTest::newRow("sos") << QByteArray("\x1bXhidden\x1b\\");
        QTest::newRow("pm") << QByteArray("\x1b^hidden\x1b\\");
        QTest::newRow("apc") << QByteArray("\x1b_hidden\x1b\\");
    }
    void ansiIntermediateAndControlStringsAreStripped() {
        QFETCH(QByteArray, control);
        Fixture f;
        f.add("err"); for (const auto byte : control) f.add(QByteArray(1, byte)); f.add("or\n"); f.grant();
        HistorySearchService service(f.port(), f.authorization); HistorySearchRequest q; q.captureId = f.id; q.query = "error";
        const auto page = search(service, q, f.context); QVERIFY(page.error.isEmpty()); QVERIFY(page.complete);
        QCOMPARE(page.hits.size(), size_t(1)); QCOMPARE(page.hits.front().ranges.size(), size_t(2));
        QCOMPARE(page.hits.front().ranges.front().recordId, quint64(1));
        QCOMPARE(page.hits.front().ranges.back().recordId, quint64(control.size() + 2));
        q.query = "hidden"; const auto hidden = search(service, q, f.context);
        QVERIFY(hidden.error.isEmpty()); QVERIFY(hidden.hits.empty()); QVERIFY(hidden.complete);
    }
    void controlStringLimitStaysExplicit() {
        Fixture f; f.add(QByteArray("\x1bP") + QByteArray(4096, 'x') + "\x1b\\\n"); f.grant();
        HistorySearchService service(f.port(), f.authorization); HistorySearchRequest q; q.captureId = f.id; q.query = "missing";
        const auto page = search(service, q, f.context);
        QVERIFY(page.error.isEmpty()); QVERIFY(!page.complete); QVERIFY(page.nextCursor.isEmpty());
        QCOMPARE(page.reason, QString("PROJECTION_LIMIT"));
    }
    void clippedCoalescedGapIsExplicitInsteadOfSkipped() {
        Fixture f; HistoryRecord gap; gap.captureId = f.id; gap.recordId = 1; gap.lastRecordId = 20;
        gap.kind = RecordKind::Gap; gap.lostRecordCount = 20; gap.reason = "outside-grant metadata";
        HistoryRecord payload; payload.captureId = f.id; payload.recordId = payload.lastRecordId = 21; payload.payload = "after gap";
        auto grant = f.grant(); grant.firstRecordId = 5; grant.lastRecordId = 25; f.authorization.setGrant(grant);
        auto port = f.port();
        port.describe = [&](HistoryCaptureId) -> std::optional<HistoryResource> {
            CaptureDescriptor descriptor; descriptor.id = f.id; descriptor.highWaterRecordId = 21;
            return HistoryResource{descriptor, 1};
        };
        port.readCommitted = [gap, payload](HistoryCaptureId, HistoryRange, auto callback) {
            ReadResult result; result.highWaterRecordId = 21; result.records = {gap, payload}; callback(std::move(result)); return true;
        };
        HistoryReadService service(std::move(port), f.authorization); HistoryReadRequest request; request.captureId = f.id;
        const auto page = read(service, request, f.context);
        QCOMPARE(page.error, QString("HISTORY_RANGE_MISSING")); QVERIFY(page.records.empty()); QCOMPARE(page.byteCount, qsizetype(0));
    }
    void projectionRetainsOffsetsAcrossRepeatedLineDrainAndCrOverwrite() {
        HistoryProjectionDecoder decoder; QElapsedTimer timer; timer.start();
        for (quint64 recordId = 1; recordId <= 64; ++recordId) {
            for (qsizetype i = 0; i < 3000; ++i) decoder.feed('a', recordId, i);
            decoder.feed('\r', recordId, 3000); decoder.feed('x', recordId, 3001); decoder.feed('\n', recordId, 3002);
            QVERIFY(!decoder.limited()); const auto line = decoder.takeReady(); QCOMPARE(line.size(), size_t(3001));
            QCOMPARE(line.front().value, U'x'); QCOMPARE(line.front().ranges.front().byteOffset, qsizetype(3001));
            QCOMPARE(line[1].value, U'a'); QCOMPARE(line[1].ranges.front().byteOffset, qsizetype(1));
            QCOMPARE(line.back().value, U'\n'); QVERIFY(decoder.memoryBytes() < 4096);
        }
        qInfo() << "Projected 192000 text bytes plus CR overwrite in" << timer.elapsed() << "ms";
    }
    void readMoreThan128PagesKeepsOnlyCurrentRetryWindow() {
        Fixture f; f.add(QByteArray(180, 'z')); f.grant(); HistoryReadService service(f.port(), f.authorization);
        HistoryReadRequest request; request.captureId = f.id; request.maxPayloadBytes = 1;
        QByteArray output; QString oldestCursor;
        for (int i = 0; i < 180; ++i) {
            const auto page = read(service, request, f.context);
            QVERIFY2(page.error.isEmpty(), qPrintable(page.error)); QCOMPARE(page.byteCount, qsizetype(1));
            QCOMPARE(page.highWaterRecordId, quint64(1)); QCOMPARE(page.records.size(), size_t(1));
            QCOMPARE(page.records.front().byteOffset, qsizetype(i)); output += page.records.front().record.payload;
            if (i == 0) { oldestCursor = page.nextCursor; f.add("new high water must stay excluded"); }
            if (i > 128) {
                const auto retry = read(service, request, f.context);
                QVERIFY(retry.error.isEmpty()); QCOMPARE(retry.nextCursor, page.nextCursor);
                QCOMPARE(retry.records.front().byteOffset, qsizetype(i));
            }
            request.cursor = page.nextCursor;
            if (i < 179) QVERIFY(!request.cursor.isEmpty()); else QVERIFY(request.cursor.isEmpty());
        }
        QCOMPARE(output, QByteArray(180, 'z'));
        request.cursor = oldestCursor; QCOMPARE(read(service, request, f.context).error, QString("CURSOR_EXPIRED"));
        request.cursor.clear(); QVERIFY(read(service, request, f.context).error.isEmpty());
    }
    void concurrentReadOfSameCursorIsBusy() {
        Fixture f; f.add("three pages"); f.grant(); auto port = f.port(); const auto sourceRead = port.readCommitted;
        port.readCommitted = [sourceRead](auto id, auto range, auto callback) {
            return sourceRead(id, range, [callback](ReadResult result) {
                QTimer::singleShot(10, [callback, result = std::move(result)]() mutable { callback(std::move(result)); });
            });
        };
        HistoryReadService service(std::move(port), f.authorization);
        HistoryReadRequest request; request.captureId = f.id; request.maxPayloadBytes = 1;
        request.cursor = read(service, request, f.context).nextCursor; QVERIFY(!request.cursor.isEmpty());
        std::optional<HistoryReadPage> first, second;
        service.readAsync(request, f.context, [&](auto result) { first = std::move(result); });
        service.readAsync(request, f.context, [&](auto result) { second = std::move(result); });
        QTRY_VERIFY(first.has_value() && second.has_value());
        QVERIFY(first->error.isEmpty()); QCOMPARE(second->error, QString("BUSY"));
        const auto retry = read(service, request, f.context);
        QCOMPARE(retry.nextCursor, first->nextCursor); QCOMPARE(retry.records.front().byteOffset, qsizetype(1));
    }
    void incompleteUtf8QueryIsRejectedBeforeCancellation() {
        Fixture f; f.add("payload\n"); f.grant(); HistorySearchService service(f.port(), f.authorization);
        for (const auto& bytes : {QByteArray::fromHex("e4"), QByteArray::fromHex("f09f"), QByteArray::fromHex("61e4")}) {
            HistorySearchRequest request; request.captureId = f.id; request.query = bytes;
            request.cancelled = std::make_shared<std::atomic<bool>>(true);
            QCOMPARE(search(service, request, f.context).error, QString("INVALID_ARGUMENT"));
        }
    }
    void expiredCursorDoesNotRestartScan() {
        Fixture f; f.add("incomplete scan"); f.grant(); auto port = f.port(); const auto sourceRead = port.readCommitted;
        int reads = 0; port.readCommitted = [&](auto id, auto range, auto callback) { ++reads; return sourceRead(id, range, callback); };
        HistorySearchLimits limits; limits.idleMilliseconds = 100;
        HistorySearchService service(std::move(port), f.authorization, nullptr, limits);
        HistorySearchRequest request; request.captureId = f.id; request.query = "missing"; request.scanBytes = 1;
        request.cursor = search(service, request, f.context).nextCursor; QVERIFY(!request.cursor.isEmpty());
        const auto readCount = reads; QTest::qWait(150);
        QCOMPARE(search(service, request, f.context).error, QString("SEARCH_CURSOR_EXPIRED")); QCOMPARE(reads, readCount);
    }
    void initiallyCancelledSearchDoesNotReadSource() {
        Fixture f; f.add("payload"); f.grant(); auto port = f.port(); const auto sourceRead = port.readCommitted;
        int reads = 0; port.readCommitted = [&](auto id, auto range, auto callback) { ++reads; return sourceRead(id, range, callback); };
        HistorySearchService service(std::move(port), f.authorization);
        HistorySearchRequest request; request.captureId = f.id; request.query = "payload";
        request.cancelled = std::make_shared<std::atomic<bool>>(true);
        QCOMPARE(search(service, request, f.context).error, QString("CANCELLED")); QCOMPARE(reads, 0);
    }
    void pagedMatchesEqualContinuousScan() {
        for (const auto mode : {HistorySearchMode::Text, HistorySearchMode::RawBytes}) {
            Fixture f; f.add("ab ab ab ab\n"); f.add("ab end\n"); f.grant();
            HistorySearchService service(f.port(), f.authorization);
            HistorySearchRequest q; q.captureId = f.id; q.query = "ab"; q.mode = mode; q.maxMatches = 1;
            size_t matches = 0; bool complete = false;
            for (int page = 0; page < 12; ++page) {
                const auto result = search(service, q, f.context);
                QVERIFY2(result.error.isEmpty(), qPrintable(result.error)); matches += result.hits.size();
                if (result.complete) { complete = true; break; }
                QVERIFY(!result.nextCursor.isEmpty()); q.cursor = result.nextCursor;
            }
            QVERIFY(complete); QCOMPARE(matches, size_t(5));
        }
    }
    void readBinaryPagesUseOffsetsAndFixedHighWater() {
        Fixture f; f.add(QByteArray(8000, '\0')); f.grant(); HistoryReadService service(f.port(), f.authorization);
        HistoryReadRequest q; q.captureId = f.id; q.maxResponseBytes = 2048;
        auto p = read(service, q, f.context); QVERIFY(p.error.isEmpty()); QVERIFY(p.byteCount > 0); QVERIFY(p.truncated);
        QCOMPARE(p.highWaterRecordId, quint64(1)); QByteArray bytes = p.records.front().record.payload;
        f.add("appended"); const auto firstBytes = p.byteCount;
        while (!p.nextCursor.isEmpty()) {
            q.cursor = p.nextCursor; p = read(service, q, f.context); QVERIFY(p.error.isEmpty());
            QCOMPARE(p.highWaterRecordId, quint64(1));
            for (const auto& fragment : p.records) { QCOMPARE(fragment.byteOffset, bytes.size()); bytes += fragment.record.payload; }
        }
        QVERIFY(firstBytes < 8000); QCOMPARE(bytes, QByteArray(8000, '\0'));
    }
    void rotatedTokenInvalidatesCursor() {
        Fixture f; f.add(QByteArray(8000, 'x')); f.grant(); HistoryReadService service(f.port(), f.authorization);
        HistoryReadRequest q; q.captureId = f.id; q.maxResponseBytes = 2048;
        const auto first = read(service, q, f.context); QVERIFY(!first.nextCursor.isEmpty()); q.cursor = first.nextCursor;
        auto rotated = f.context; ++rotated.tokenEpoch;
        QCOMPARE(read(service, q, rotated).error, QString("PERMISSION_DENIED"));
    }
    void rangeShrinkRejectsOldCursor() {
        Fixture f; f.add(QByteArray(8000, 'x')); f.add("secret"); auto grant = f.grant(); HistoryReadService service(f.port(), f.authorization);
        HistoryReadRequest q; q.captureId = f.id; q.maxResponseBytes = 2048; q.cursor = read(service, q, f.context).nextCursor;
        QVERIFY(!q.cursor.isEmpty()); grant.lastRecordId = 1; f.authorization.setGrant(grant);
        QCOMPARE(read(service, q, f.context).error, QString("PERMISSION_DENIED"));
    }
    void literalAcrossChunk() {
        Fixture f; f.add("first err"); f.add("or last\n"); f.grant(); HistorySearchService service(f.port(), f.authorization);
        HistorySearchRequest q; q.captureId = f.id; q.query = "error";
        const auto p = search(service, q, f.context); QVERIFY2(p.error.isEmpty(), qPrintable(p.error)); QVERIFY(p.complete);
        QCOMPARE(p.hits.size(), size_t(1)); QCOMPARE(p.hits[0].ranges.size(), size_t(2));
        QCOMPARE(p.hits[0].ranges[0].recordId, quint64(1)); QCOMPARE(p.hits[0].ranges[0].byteOffset, qsizetype(6));
        QCOMPARE(p.hits[0].ranges[0].byteCount, qsizetype(3)); QCOMPARE(p.hits[0].ranges[1].byteCount, qsizetype(2));
    }
    void splitUtf8AndAnsi() {
        Fixture f; f.add(QByteArray::fromHex("e4")); f.add(QByteArray::fromHex("b8ad1b5b33")); f.add("1mOK\n"); f.grant();
        HistorySearchService service(f.port(), f.authorization); HistorySearchRequest q; q.captureId = f.id; q.query = QString::fromUtf8("中OK").toUtf8();
        const auto p = search(service, q, f.context); QVERIFY(p.complete); QCOMPARE(p.hits.size(), size_t(1));
        QCOMPARE(p.hits[0].ranges.size(), size_t(3)); QCOMPARE(p.hits[0].ranges[0].byteCount, qsizetype(1));
        QCOMPARE(p.hits[0].ranges[1].byteCount, qsizetype(2)); QCOMPARE(p.hits[0].ranges[2].byteOffset, qsizetype(2));
    }
    void crProjectionMapsToRaw() {
        Fixture f; f.add("abc\rX\n"); f.grant(); HistorySearchService service(f.port(), f.authorization);
        HistorySearchRequest q; q.captureId = f.id; q.query = "Xbc";
        const auto p = search(service, q, f.context); QCOMPARE(p.hits.size(), size_t(1));
        QCOMPARE(p.hits[0].ranges.size(), size_t(2)); QCOMPARE(p.hits[0].ranges[0].byteOffset, qsizetype(4));
        QCOMPARE(p.hits[0].ranges[1].byteOffset, qsizetype(1)); QCOMPARE(p.hits[0].ranges[1].byteCount, qsizetype(2));
    }
    void matchDoesNotCrossGapOrBoot() {
        for (const auto kind : {RecordKind::Gap, RecordKind::Boundary}) {
            Fixture f; f.add("err"); f.add({}, kind); f.add("or\n"); f.grant(); HistorySearchService service(f.port(), f.authorization);
            for (const auto mode : {HistorySearchMode::RawBytes, HistorySearchMode::Text}) {
                HistorySearchRequest q; q.captureId = f.id; q.query = "error"; q.mode = mode;
                const auto p = search(service, q, f.context); QVERIFY(p.complete); QVERIFY(p.hits.empty());
            }
        }
    }
    void budgetInsideUtf8AnsiAndLiteral() {
        Fixture f; f.add(QByteArray::fromHex("e4b8ad1b5b33316d6572726f720a")); f.grant(); HistorySearchService service(f.port(), f.authorization);
        HistorySearchRequest q; q.captureId = f.id; q.query = QString::fromUtf8("中error").toUtf8(); q.scanBytes = 1;
        size_t matches = 0; bool complete = false;
        for (int i = 0; i < 30; ++i) {
            const auto p = search(service, q, f.context); QVERIFY2(p.error.isEmpty(), qPrintable(p.error)); matches += p.hits.size();
            if (p.complete) { complete = true; break; }
            QVERIFY(!p.nextCursor.isEmpty()); q.cursor = p.nextCursor;
        }
        QVERIFY(complete); QCOMPARE(matches, size_t(1));
    }
    void changedQueryRejectsCursor() {
        Fixture f; f.add("a long incomplete line"); f.grant(); HistorySearchService service(f.port(), f.authorization);
        HistorySearchRequest q; q.captureId = f.id; q.query = "long"; q.scanBytes = 1;
        q.cursor = search(service, q, f.context).nextCursor; QVERIFY(!q.cursor.isEmpty()); q.query = "other";
        QCOMPARE(search(service, q, f.context).error, QString("CURSOR_MISMATCH"));
    }
    void duplicatePageDoesNotAdvance() {
        Fixture f; f.add("ab ab ab ab"); f.grant(); HistorySearchService service(f.port(), f.authorization);
        HistorySearchRequest q; q.captureId = f.id; q.query = "ab"; q.mode = HistorySearchMode::RawBytes; q.scanBytes = 3;
        q.cursor = search(service, q, f.context).nextCursor; QVERIFY(!q.cursor.isEmpty());
        const auto a = search(service, q, f.context); const auto b = search(service, q, f.context);
        QCOMPARE(a.hits.size(), size_t(1)); QCOMPARE(b.hits.size(), a.hits.size()); QCOMPARE(b.nextCursor, a.nextCursor);
        QCOMPARE(b.hits[0].ranges[0].byteOffset, a.hits[0].ranges[0].byteOffset);
    }
    void longLineProjectionLimitIsExplicit() {
        Fixture f; f.add(QByteArray(70 * 1024, 'a')); f.grant(); HistorySearchService service(f.port(), f.authorization);
        HistorySearchRequest q; q.captureId = f.id; q.query = "missing";
        const auto p = search(service, q, f.context); QVERIFY(!p.complete); QVERIFY(p.nextCursor.isEmpty());
        QCOMPARE(p.reason, QString("PROJECTION_LIMIT")); QVERIFY(!p.projectionGaps.empty());
    }
    void cancellationAndPrincipalStateLimit() {
        Fixture f; f.add("incomplete"); f.grant(); HistorySearchService service(f.port(), f.authorization);
        HistorySearchRequest q; q.captureId = f.id; q.query = "missing"; q.scanBytes = 1;
        const auto a = search(service, q, f.context); const auto b = search(service, q, f.context);
        QVERIFY(!a.nextCursor.isEmpty()); QVERIFY(!b.nextCursor.isEmpty());
        QCOMPARE(search(service, q, f.context).error, QString("BUSY"));
        QVERIFY(!service.cancel(a.nextCursor, {"different", 7})); QVERIFY(service.cancel(a.nextCursor, f.context));
        q.cursor = a.nextCursor; QCOMPARE(search(service, q, f.context).error, QString("SEARCH_CURSOR_EXPIRED"));
    }
    void deletionAndRevocationAreCheckedBeforeDelivery() {
        Fixture f; f.add("incomplete"); f.grant(); HistorySearchService service(f.port(), f.authorization);
        HistorySearchRequest q; q.captureId = f.id; q.query = "missing"; q.scanBytes = 1;
        q.cursor = search(service, q, f.context).nextCursor; f.exists = false;
        QCOMPARE(search(service, q, f.context).error, QString("EVIDENCE_EXPIRED")); f.exists = true;
        f.authorization.revoke(f.context.principalId, f.id);
        QCOMPARE(search(service, q, f.context).error, QString("PERMISSION_DENIED"));
    }
};
QTEST_GUILESS_MAIN(HistorySearchTests)
#include "HistorySearchTests.moc"
