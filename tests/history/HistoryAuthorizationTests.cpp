#include "history/HistoryAuthorizationStore.h"
#include <QtTest>
using namespace NovaTerm::Recording;
class HistoryAuthorizationTests : public QObject {
    Q_OBJECT
private slots:
    void closedSessionExplicitCaptureGrantWorks() {
        HistoryAuthorizationStore store; const HistoryCaptureId id{QUuid::createUuid()};
        ResourceGrant grant; grant.principalId = "client"; grant.tokenEpoch = 3; grant.captureId = id;
        const auto saved = store.setGrant(grant);
        QVERIFY(saved.accessVersion > 0);
        QVERIFY(store.authorize({"client", 3}, id, 1).has_value());
        QVERIFY(!store.authorize({"other", 3}, id, 1));
    }
    void sessionGrantDoesNotAuthorizeHistory() {
        HistoryAuthorizationStore store;
        QVERIFY(!store.authorize({"client", 1}, {QUuid::createUuid()}, 1));
        QVERIFY(store.grants({"client", 1}).empty());
    }
    void rotationAndRangeShrinkInvalidateVersion() {
        HistoryAuthorizationStore store; ResourceGrant g; g.captureId = {QUuid::createUuid()}; g.principalId = "c"; g.tokenEpoch = 4;
        const auto before = store.setGrant(g); g.lastRecordId = 2; const auto after = store.setGrant(g);
        QVERIFY(before.accessVersion != after.accessVersion);
        QVERIFY(!store.authorize({"c", 5}, g.captureId, 1));
        QVERIFY(!store.authorize({"c", 4}, g.captureId, 2));
        HistoryRecord r; r.recordId = 3; r.lastRecordId = 3;
        QVERIFY(!HistoryAuthorizationStore::allowsRecord(after, r));
        r.recordId = r.lastRecordId = 2; QVERIFY(HistoryAuthorizationStore::allowsRecord(after, r));
        store.revoke("c", g.captureId); QVERIFY(!store.authorize({"c", 4}, g.captureId, 1));
    }
    void timeBoundsAndCoalescedGapsAreNotExpanded() {
        ResourceGrant g; g.firstRecordId = 2; g.lastRecordId = 4; g.firstTimestampMs = 10; g.lastTimestampMs = 20;
        HistoryRecord r; r.recordId = 2; r.lastRecordId = 4; r.wallTimestampMs = 15;
        QVERIFY(HistoryAuthorizationStore::allowsRecord(g, r));
        r.wallTimestampMs = 9; QVERIFY(!HistoryAuthorizationStore::allowsRecord(g, r));
        r.wallTimestampMs = 15; r.lastRecordId = 5; QVERIFY(!HistoryAuthorizationStore::allowsRecord(g, r));
    }
};
QTEST_GUILESS_MAIN(HistoryAuthorizationTests)
#include "HistoryAuthorizationTests.moc"
