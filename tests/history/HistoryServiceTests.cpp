#include "history/HistoryService.h"
#include "history/HistoryChunkStore.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <atomic>
#include <thread>

using namespace NovaTerm::Recording;
class HistoryServiceTests : public QObject {
    Q_OBJECT
private slots:
    void invalidRetentionRequiresExplicitRepair() {
        QTemporaryDir dir; { QFile f(dir.path()+"/retention.json"); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("[]"); }
        HistoryService service(dir.path()); const auto session = QUuid::createUuid(); bool replied = false;
        QVERIFY(service.start({session, 1}, [&](auto result) { QVERIFY(!result.success); replied = true; })); QTRY_VERIFY(replied);
        QVERIFY(!service.error().isEmpty()); replied = false;
        QVERIFY(service.configure({4 * 1024 * 1024, 2 * 1024 * 1024, 86400000}, [&](auto result) {
            QVERIFY(result.success); QCOMPARE(service.settings().captureBytes, quint64(2 * 1024 * 1024)); replied = true;
        })); QTRY_VERIFY(replied); QVERIFY(service.error().isEmpty()); replied = false;
        QVERIFY(service.start({session, 1}, [&](auto result) { QVERIFY(result.success); replied = true; })); QTRY_VERIFY(replied);
    }
    void raisedQuotaReachesActiveStore() {
        QTemporaryDir dir; HistoryService service(dir.path()); const auto session = QUuid::createUuid(); bool done = false;
        QVERIFY(service.configure({4 * 1024 * 1024, 1024 * 1024, 86400000}, [&](auto r) { QVERIFY(r.success); done = true; })); QTRY_VERIFY(done);
        done = false; QVERIFY(service.start({session, 1}, [&](auto r) { QVERIFY(r.success); done = true; })); QTRY_VERIFY(done);
        done = false; QVERIFY(service.configure({4 * 1024 * 1024, 2 * 1024 * 1024, 86400000}, [&](auto r) { QVERIFY(r.success); done = true; })); QTRY_VERIFY(done);
        for (int i = 0; i < 24; ++i) QCOMPARE(service.submit(session, 1, QByteArray(64 * 1024, 'x')), SubmitResult::Accepted);
        service.stop({session, 1}); QTRY_VERIFY(!service.captures().front().active);
        QCOMPARE(service.captures().front().integrity, Integrity::Lossless);
        QCOMPARE(service.captures().front().highWaterRecordId, quint64(24));
    }
    void failedRecoveryPreservesKeptCapture() {
        QTemporaryDir dir; CaptureDescriptor d; d.id = {QUuid::createUuid()}; d.sessionId = QUuid::createUuid();
        { HistoryChunkStore s(dir.path(), d); QVERIFY(s.open()); HistoryRecord r; r.recordId = 1; r.payload = "kept";
          QVERIFY(s.append({r})); QVERIFY(s.commit()); QVERIFY(s.seal("stop")); }
        const auto path = dir.path()+"/"+d.id.text();
        { QFile marker(path+"/retained"); QVERIFY(marker.open(QIODevice::WriteOnly)); marker.write("kept"); }
        { QFile padding(path+"/reserved.bin"); QVERIFY(padding.open(QIODevice::WriteOnly)); QVERIFY(padding.resize(2 * 1024 * 1024)); }
        { QFile settings(dir.path()+"/retention.json"); QVERIFY(settings.open(QIODevice::WriteOnly)); settings.write("{\"global\":\"1048576\",\"capture\":\"1048576\",\"ttl\":\"86400000\"}"); }
        bool replied = false; HistoryService service(dir.path(), nullptr, [](IoPoint p) { return p != IoPoint::RecoveryScan; });
        QVERIFY(service.start({QUuid::createUuid(), 1}, [&](auto) { replied = true; })); QTRY_VERIFY(replied);
        QVERIFY(QFileInfo::exists(path+"/retained")); QVERIFY(QFileInfo::exists(path+"/chunks/00000001.bin"));
        QVERIFY(!service.captures().empty()); QVERIFY(service.captures().front().retained);
    }
    void destructionWaitsForSqlOwnerToExit() {
        struct State { std::atomic<bool> entered{false}, left{false}; };
        const auto state = std::make_shared<State>(); QTemporaryDir dir;
        auto service = std::make_unique<HistoryService>(dir.path(), nullptr, [state](IoPoint p) {
            if (p == IoPoint::DataWrite && !state->entered.exchange(true)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(900)); state->left = true;
            } return true;
        });
        const auto session = QUuid::createUuid(); bool started = false;
        QVERIFY(service->start({session, 1}, [&](auto r) { QVERIFY(r.success); started = true; })); QTRY_VERIFY(started);
        service->submit(session, 1, "pending"); QTRY_VERIFY(state->entered.load());
        service.reset(); const bool exitedAtReturn = state->left.load();
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(dir.path()+"/history.lock"), 5000);
        QVERIFY(exitedAtReturn);
    }
    void idleCaptureDoesNotRescanItsDirectory() {
        QTemporaryDir dir; std::atomic<int> scans{0}; const auto session = QUuid::createUuid(); bool started = false;
        HistoryService service(dir.path(), nullptr, [&](IoPoint p) { if (p == IoPoint::DirectoryScan) ++scans; return true; });
        QVERIFY(service.start({session, 1}, [&](auto r) { QVERIFY(r.success); started = true; })); QTRY_VERIFY(started);
        service.submit(session, 1, "raw"); QTRY_VERIFY(service.captures().front().highWaterRecordId > 0);
        QVERIFY(scans > 0); const int baseline = scans.load(); QTest::qWait(150); QCOMPARE(scans.load(), baseline);
    }
    void captureQuotaStopsWithoutFalseSeal() {
        QTemporaryDir dir; HistoryService service(dir.path()); bool configured = false;
        QVERIFY(service.configure({2 * 1024 * 1024, 1024 * 1024, 7ll * 24 * 60 * 60 * 1000},
            [&](auto r) { QVERIFY(r.success); configured = true; })); QTRY_VERIFY(configured);
        const auto session = QUuid::createUuid(); bool started = false; HistoryCaptureId id;
        QVERIFY(service.start({session, 1}, [&](auto r) { QVERIFY(r.success); started = true; id = r.captureId; })); QTRY_VERIFY(started);
        for (int i = 0; i < 24; ++i) service.submit(session, 1, QByteArray(64 * 1024, 'x'));
        QTRY_VERIFY(!service.captures().front().active);
        QCOMPARE(service.captures().front().integrity, Integrity::UnknownTail);
        QCOMPARE(service.captures().front().stopReason, QString("capture_quota"));
        QVERIFY(service.captures().front().bytes <= 1024 * 1024);
        bool read = false; QVERIFY(service.readCommitted(id, {}, [&](auto r) { QCOMPARE(r.integrity, Integrity::UnknownTail); QVERIFY(!r.lostByteCount); read = true; })); QTRY_VERIFY(read);
    }
    void offlinePageDoesNotRescanOrRebuildCapture() {
        QTemporaryDir dir; std::atomic<bool> allowScan{true};
        HistoryService service(dir.path(), nullptr, [&](IoPoint point) { return point != IoPoint::RecoveryScan || allowScan.load(); });
        const auto session = QUuid::createUuid(); bool started = false; HistoryCaptureId id;
        QVERIFY(service.start({session, 1}, [&](auto r) { QVERIFY(r.success); started = true; id = r.captureId; })); QTRY_VERIFY(started);
        service.submit(session, 1, "raw"); service.stop({session, 1}); QTRY_VERIFY(!service.captures().front().active);
        allowScan = false; bool read = false;
        QVERIFY(service.readCommitted(id, {}, [&](auto r) { QVERIFY2(r.error.isEmpty(), qPrintable(r.error)); QCOMPARE(r.records.size(), size_t(1)); read = true; }));
        QTRY_VERIFY(read);
    }
    void secondOwnerCannotReclassifyActiveCapture() {
        QTemporaryDir dir; HistoryService first(dir.path()); const auto session = QUuid::createUuid(); bool started = false;
        QVERIFY(first.start({session, 1}, [&](auto r) { QVERIFY(r.success); started = true; })); QTRY_VERIFY(started);
        HistoryService second(dir.path()); bool rejected = false;
        QVERIFY(second.start({QUuid::createUuid(), 1}, [&](auto r) { QVERIFY(!r.success); rejected = true; })); QTRY_VERIFY(rejected);
        QVERIFY(second.captures().empty()); QCOMPARE(first.submit(session, 1, "still active"), SubmitResult::Accepted);
    }
    void disabledUntilLocalStartAndSealedAfterStop() {
        QTemporaryDir dir; HistoryService service(dir.path()); const auto session = QUuid::createUuid();
        QCOMPARE(service.submit(session, 1, "off"), SubmitResult::Disabled);
        bool started = false; HistoryCaptureId id;
        QVERIFY(service.start({session, 1}, [&](auto r) { QVERIFY2(r.success, qPrintable(r.error)); id = r.captureId; started = true; }));
        QTRY_VERIFY(started);
        QCOMPARE(service.submit(session, 1, "raw"), SubmitResult::Accepted);
        QCOMPARE(service.submit(session, 2, "stale"), SubmitResult::Disabled);
        service.stop({session, 1});
        QTRY_VERIFY(!service.captures().empty() && !service.captures().front().active);
        bool read = false;
        QVERIFY(service.readCommitted(id, {}, [&](auto r) { QCOMPARE(r.integrity, Integrity::Lossless); QCOMPARE(r.records.size(), size_t(1)); QCOMPARE(r.records[0].payload, QByteArray("raw")); read = true; }));
        QTRY_VERIFY(read);
        bool removed = false; QVERIFY(service.remove(id, [&](auto r) { QVERIFY(r.success); removed = true; })); QTRY_VERIFY(removed);
        QVERIFY(service.readCommitted(id, {}, [&](auto r) { QVERIFY(r.missing); read = false; })); QTRY_VERIFY(!read);
    }
    void stopDuringStartDoesNotInstallProducer() {
        QTemporaryDir dir; HistoryService service(dir.path()); const auto session = QUuid::createUuid(); bool replied = false;
        QVERIFY(service.start({session, 1}, [&](auto) { replied = true; }));
        service.stop({session, 1}, "connection_changed"); QTRY_VERIFY(replied);
        QCOMPARE(service.submit(session, 1, "late"), SubmitResult::Disabled);
        QTRY_VERIFY(service.captures().empty() || !service.captures().front().active);
    }
    void queuedBytesLostAreReported() {
        QTemporaryDir dir; HistoryService service(dir.path()); const auto session = QUuid::createUuid(); bool started = false; HistoryCaptureId id;
        QVERIFY(service.start({session, 1}, [&](auto r) { QVERIFY(r.success); id = r.captureId; started = true; })); QTRY_VERIFY(started);
        QCOMPARE(service.submit(session, 1, QByteArray(17 * 1024 * 1024, 'x')), SubmitResult::GapRecorded);
        service.stop({session, 1}); QTRY_VERIFY(!service.captures().front().active);
        bool read = false; QVERIFY(service.readCommitted(id, {}, [&](auto r) {
            QCOMPARE(r.integrity, Integrity::KnownGap); QCOMPARE(r.lostByteCount.value(), quint64(17 * 1024 * 1024)); read = true;
        })); QTRY_VERIFY(read);
    }
};
QTEST_GUILESS_MAIN(HistoryServiceTests)
#include "HistoryServiceTests.moc"
