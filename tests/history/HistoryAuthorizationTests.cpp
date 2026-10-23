#include "history/HistoryAuthorizationStore.h"
#include <QTest>
using namespace NovaTerm::Recording;
class HistoryAuthorizationTests : public QObject {
    Q_OBJECT
private slots:
    void closedSessionExplicitCaptureGrantWorks() {
        HistoryAuthorizationStore store; HistoryCaptureId id{QUuid::createUuid()};
        QVERIFY(!store.authorize({"client", "token"}, id));
        QVERIFY(store.share({"client", "token", id, 2, 8}));
        const auto grant = store.authorize({"client", "token"}, id);
        QVERIFY(grant); QCOMPARE(grant->first, 2ull); QCOMPARE(grant->last, 8ull);
        QVERIFY(!store.authorize({"other", "token"}, id));
    }
    void rotatedTokenAndShrinkInvalidateVersion() {
        HistoryAuthorizationStore store; HistoryCaptureId id{QUuid::createUuid()};
        QVERIFY(store.share({"client", "token", id}));
        const auto old = store.authorize({"client", "token"}, id)->version;
        QVERIFY(!store.authorize({"client", "rotated"}, id));
        QVERIFY(store.share({"client", "token", id, 3, 4}));
        QVERIFY(store.authorize({"client", "token"}, id)->version != old);
        store.revoke("client", id); QVERIFY(!store.authorize({"client", "token"}, id));
    }
    void onlyExplicitPersistentGrantsSurviveRestart() {
        HistoryAuthorizationStore store, restored; HistoryCaptureId a{QUuid::createUuid()}, b{QUuid::createUuid()};
        ResourceGrant grant{"client", "token", a}; grant.persistent = true;
        QVERIFY(store.share(grant)); grant.capture = b; grant.persistent = false; QVERIFY(store.share(grant));
        QVERIFY(restored.restore(store.persistentState()));
        QVERIFY(restored.authorize({"client", "token"}, a));
        QVERIFY(!restored.authorize({"client", "token"}, b));
    }
};
QTEST_GUILESS_MAIN(HistoryAuthorizationTests)
#include "HistoryAuthorizationTests.moc"
