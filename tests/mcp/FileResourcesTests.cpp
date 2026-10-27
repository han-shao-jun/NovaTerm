#include "mcp/application/FileResources.h"
#include "mcp/contracts/Budget.h"
#include <QDir>
#include <QTest>

using namespace NovaTerm::Mcp;
namespace {
QString localPath(const QString& tail = "artifacts") { return QDir::cleanPath(QDir::currentPath() + '/' + tail); }
SessionBinding binding()
{
    return {SessionId{"session-a"}, AttachmentId{"attachment-a"}, Epoch{"epoch-a"}, Generation{1}, QString("verified-target")};
}
}
class FileResourcesTests final : public QObject {
    Q_OBJECT
private slots:
    void localAndRemoteAreIndependentAndVersioned()
    {
        QMap<QString, quint64> identities{{"alice", 1}, {"bob", 1}};
        FileResources resources([&identities](const QString& client) { return identities.value(client); });
        const auto local = resources.shareLocal("alice", 1, localPath(), "Build", {true, false});
        QVERIFY(local); QVERIFY(local->access.read); QVERIFY(!local->access.write);
        QVERIFY(resources.local("bob", 1).isEmpty());
        QVERIFY(resources.remote("alice", 1, binding()).isEmpty());
        const auto remote = resources.shareRemote("alice", 1, binding(), "/tmp/nvt", "Target", {false, true});
        QVERIFY(remote); QVERIFY(!remote->access.read); QVERIFY(remote->access.write);
        QCOMPARE(resources.remote("alice", 1, binding()).size(), 1);
        const auto updated = resources.shareLocal("alice", 1, localPath(), "Download", {false, true});
        QVERIFY(updated); QCOMPARE(updated->id, local->id); QVERIFY(updated->revision != local->revision);
        QVERIFY(!resources.remove("alice", 1, local->id, local->revision));
        QVERIFY(!resources.remove("bob", 1, updated->id, updated->revision));
        QVERIFY(resources.remove("alice", 1, updated->id, updated->revision));
        const auto again = resources.shareLocal("alice", 1, localPath(), "Build", {true, false});
        QVERIFY(again); QVERIFY(again->id != local->id);
        identities["alice"] = 2;
        QVERIFY(resources.local("alice", 1).isEmpty());
        QVERIFY(resources.local("alice", 2).isEmpty());
        QVERIFY(!resources.shareLocal("alice", 1, localPath(), "Old UI", {true, true}));
        QVERIFY(resources.remote("alice", 2, binding()).isEmpty());
    }
    void everyConnectionComponentIsRequired()
    {
        FileResources resources([](const QString&) { return 1; });
        QVERIFY(resources.shareRemote("alice", 1, binding(), "/tmp/nvt", "Target", {true, true}));
        for (int field = 0; field < 5; ++field) {
            auto changed = binding();
            if (field == 0) changed.session = SessionId{"other"};
            if (field == 1) changed.attachment = AttachmentId{"other"};
            if (field == 2) changed.epoch = Epoch{"other"};
            if (field == 3) changed.generation = Generation{2};
            if (field == 4) changed.targetFingerprint = "other";
            QVERIFY(resources.remote("alice", 1, changed).isEmpty());
        }
        auto unknown = binding(); unknown.targetFingerprint.reset();
        QVERIFY(!resources.shareRemote("alice", 1, unknown, "/tmp/other", "Target", {true, true}));
        resources.invalidateSession("session-a");
        QVERIFY(resources.remote("alice", 1, binding()).isEmpty());
    }
    void pathsAndPermissionsFailClosedWithoutIo()
    {
        FileResources resources([](const QString&) { return 1; });
        for (const auto& path : QStringList{"", ".", "../x", "relative/x", "/", "//host/share", "/tmp/../etc",
                "/tmp/./x", "/tmp/with\nnewline", "/tmp/back\\slash", QString(1025, 'a')})
            QVERIFY2(!resources.shareRemote("alice", 1, binding(), path, "Target", {true, false}), qPrintable(path));
        for (const auto& path : QStringList{"", "relative", "../x", "//server/share", "\\\\?\\C:\\folder",
                "\\\\.\\C:\\folder", "\\\\?\\UNC\\server\\share", localPath("../outside") + "/../escape"})
            QVERIFY2(!resources.shareLocal("alice", 1, path, "Build", {true, false}), qPrintable(path));
        QVERIFY(!resources.shareLocal("alice", 1, localPath(), "Build", {}));
        QVERIFY(!resources.shareLocal("alice", 1, localPath(), QString(257, 'a'), {true, false}));
        QVERIFY(!resources.shareRemote("alice", 0, binding(), "/tmp/nvt", "Target", {true, false}));
        QVERIFY(resources.shareLocal("alice", 1, localPath("not-created"), "Future output", {false, true}));
    }
    void budgetsAllowUpdateAndReclaimAfterRevocation()
    {
        FileResources resources([](const QString&) { return 1; });
        for (int client = 0; client < 16; ++client)
            for (int row = 0; row < 8; ++row)
                QVERIFY(resources.shareLocal(QString::number(client), 1, localPath(QString::number(row)), "Build", {true, false}));
        QVERIFY(!resources.shareLocal("extra", 1, localPath(), "Build", {true, false}));
        QVERIFY(!resources.shareLocal("0", 1, localPath("ninth"), "Build", {true, false}));
        QVERIFY(resources.shareLocal("0", 1, localPath("0"), "Updated", {false, true}));
        resources.revoke("0");
        QVERIFY(resources.shareLocal("extra", 1, localPath(), "Build", {true, false}));
        for (int client = 0; client < 32; ++client)
            for (int row = 0; row < 8; ++row)
                QVERIFY(resources.shareRemote(QString::number(client), 1, binding(), "/tmp/nvt/" + QString::number(row), "Target", {false, true}));
        QVERIFY(!resources.shareRemote("extra", 1, binding(), "/tmp/nvt", "Target", {false, true}));
        QVERIFY(!resources.shareRemote("0", 1, binding(), "/tmp/ninth", "Target", {false, true}));
        QVERIFY(resources.shareRemote("0", 1, binding(), "/tmp/nvt/0", "Updated", {true, false}));
        resources.invalidateSession("session-a");
        QVERIFY(resources.shareRemote("extra", 1, binding(), "/tmp/nvt", "Target", {false, true}));
        resources.revoke();
        QVERIFY(resources.local("extra", 1).isEmpty());
        QVERIFY(resources.remote("extra", 1, binding()).isEmpty());
    }
    void remoteClientLimitSpansSessions()
    {
        FileResources resources([](const QString&) { return 1; });
        for (int i = 0; i < 8; ++i) {
            auto target = binding(); target.session = SessionId{QString("session-%1").arg(i)};
            QVERIFY(resources.shareRemote("alice", 1, target, "/tmp/nvt", "Target", {true, false}));
        }
        QCOMPARE(resources.remote("alice", 1).size(), 8);
        QVERIFY(!resources.shareRemote("alice", 1, binding(), "/tmp/nvt", "Ninth", {true, false}));
        resources.invalidateSession("session-0");
        QVERIFY(resources.shareRemote("alice", 1, binding(), "/tmp/nvt", "Replacement", {true, false}));
    }
};
QTEST_GUILESS_MAIN(FileResourcesTests)
#include "FileResourcesTests.moc"
