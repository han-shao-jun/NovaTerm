#include "mcp/application/SavedTargets.h"
#include <QTest>
#include <QUuid>

using namespace NovaTerm::Mcp;
namespace {
SavedTargetSource source(const QString& id = "11111111-1111-4111-8111-111111111111")
{
    return {id, "Board", "ssh", "example.org:22", QByteArray(32, 'a')};
}
}
class SavedTargetsTests final : public QObject {
    Q_OBJECT
private slots:
    void sharingIsExplicitAndBoundToIdentityAndConfiguration()
    {
        SavedTargets targets;
        QVERIFY(targets.replace({source()}));
        const auto row = targets.localTargets().first();
        QVERIFY(targets.page("alice", 1, 25).targets.isEmpty());
        QVERIFY(targets.setShared("alice", 1, row.id, row.configVersion, true));
        QCOMPARE(targets.page("alice", 1, 25).targets.size(), 1);
        QVERIFY(targets.page("bob", 1, 25).targets.isEmpty());
        QVERIFY(targets.page("alice", 2, 25).targets.isEmpty());
        QVERIFY(!targets.setShared("alice", 0, row.id, row.configVersion, true));
        auto changed = source(); changed.configurationKey[0] = 'b';
        QVERIFY(targets.replace({changed}));
        QVERIFY(targets.localTargets().first().configVersion != row.configVersion);
        QVERIFY(targets.page("alice", 1, 25).targets.isEmpty());
        QVERIFY(!targets.setShared("alice", 1, row.id, row.configVersion, true));
        const auto current = targets.localTargets().first();
        QVERIFY(targets.setShared("alice", 1, current.id, current.configVersion, true));
        targets.revoke("alice");
        QVERIFY(targets.page("alice", 1, 25).targets.isEmpty());
    }
    void pagesAreStableAndRejectMixedRevisions()
    {
        SavedTargets targets;
        auto a = source(); auto b = source("22222222-2222-4222-8222-222222222222");
        QVERIFY(targets.replace({b, a}));
        for (const auto& row : targets.localTargets())
            QVERIFY(targets.setShared("alice", 1, row.id, row.configVersion, true));
        const auto first = targets.page("alice", 1, 1);
        QCOMPARE(first.targets.first().id, a.id);
        QCOMPARE(first.nextAfter, a.id);
        const auto next = targets.page("alice", 1, 1, first.nextAfter, first.revision);
        QCOMPARE(next.targets.first().id, b.id);
        QVERIFY(next.nextAfter.isEmpty());
        const auto revision = first.revision;
        QVERIFY(targets.replace({a, b})); // unchanged source doesn't expire cursors
        QCOMPARE(targets.page("alice", 1, 1).revision, revision);
        b.endpoint = "other.example:22";
        QVERIFY(targets.replace({a, b}));
        QCOMPARE(targets.page("alice", 1, 1, first.nextAfter, revision).error,
            QString("TARGET_DIRECTORY_CHANGED"));
        QVERIFY(targets.page("alice", 1, 0).error == "INVALID_ARGUMENT");
        QVERIFY(targets.page("alice", 1, 51).error == "INVALID_ARGUMENT");
    }
    void invalidUpdatesFailClosedAndDoNotRetainOldSharing()
    {
        SavedTargets targets;
        QVERIFY(targets.replace({source()}));
        const auto row = targets.localTargets().first();
        QVERIFY(targets.setShared("alice", 1, row.id, row.configVersion, true));
        QVERIFY(!targets.replace({source(), source()}));
        QVERIFY(targets.localTargets().isEmpty());
        QVERIFY(targets.page("alice", 1, 25).targets.isEmpty());
        QVERIFY(targets.replace({source()}));
        QVERIFY(targets.page("alice", 1, 25).targets.isEmpty());
        auto invalid = source(); invalid.title = QString(129, QChar(0x4e2d));
        QVERIFY(!targets.replace({invalid}));
        invalid = source(); invalid.endpoint += '\n';
        QVERIFY(!targets.replace({invalid}));
        invalid = source(); invalid.transport = "local";
        QVERIFY(!targets.replace({invalid}));
        invalid = source(); invalid.configurationKey.clear();
        QVERIFY(!targets.replace({invalid}));
        QList<SavedTargetSource> many;
        for (int i = 0; i < 257; ++i) {
            auto item = source(QUuid::createUuid().toString(QUuid::WithoutBraces));
            many.append(item);
        }
        QVERIFY(!targets.replace(many));
        many.removeLast();
        QVERIFY(targets.replace(many));
        for (int client = 0; client < 16; ++client)
            for (const auto& item : targets.localTargets())
                QVERIFY(targets.setShared(QString::number(client), 1, item.id, item.configVersion, true));
        const auto item = targets.localTargets().first();
        QVERIFY(!targets.setShared("overflow", 1, item.id, item.configVersion, true));
        targets.revoke("0");
        QVERIFY(targets.setShared("overflow", 1, item.id, item.configVersion, true));
    }
    void deletionAndRecreationNeverResurrectGrants()
    {
        SavedTargets targets;
        QVERIFY(targets.replace({source()}));
        const auto row = targets.localTargets().first();
        QVERIFY(targets.setShared("alice", 1, row.id, row.configVersion, true));
        QVERIFY(targets.replace({}));
        QVERIFY(targets.replace({source()}));
        QVERIFY(!targets.isShared("alice", 1, row.id));
        QVERIFY(targets.localTargets().first().configVersion != row.configVersion);
    }
    void globalRevocationClearsEveryPrincipal()
    {
        SavedTargets targets;
        QVERIFY(targets.replace({source()}));
        const auto row = targets.localTargets().first();
        QVERIFY(targets.setShared("alice", 1, row.id, row.configVersion, true));
        QVERIFY(targets.setShared("bob", 1, row.id, row.configVersion, true));
        targets.revoke({});
        QVERIFY(targets.page("alice", 1, 25).targets.isEmpty());
        QVERIFY(targets.page("bob", 1, 25).targets.isEmpty());
    }
};
QTEST_GUILESS_MAIN(SavedTargetsTests)
#include "SavedTargetsTests.moc"
