#include "mcp/application/SerialFileTargets.h"
#include <QSignalSpy>
#include <QTest>

using namespace NovaTerm::Mcp;
namespace {
SerialFileBinding binding(const QString& session = "session-a")
{
    return {{SessionId{session}, AttachmentId{"attachment-a"}, Epoch{"epoch-a"}, Generation{1}, std::nullopt}, 1, "config-a"};
}
}
class SerialFileTargetsTests final : public QObject {
    Q_OBJECT
private slots:
    void grantsArePrivateVersionedAndRecreated()
    {
        QMap<QString, quint64> identities{{"alice", 1}, {"bob", 1}};
        SerialFileTargets targets([&identities](const QString& client) { return identities.value(client); });
        QSignalSpy changes(&targets, &SerialFileTargets::changed);
        const auto first = targets.share("alice", 1, binding(), "Board");
        QVERIFY(first); QCOMPARE(first->title, QString("Board"));
        QVERIFY(!first->binding.connection.targetFingerprint);
        QVERIFY(!first->id.isEmpty()); QVERIFY(!first->revision.isEmpty());
        QCOMPARE(targets.list("alice", 1).size(), 1); QVERIFY(targets.list("bob", 1).isEmpty());
        const auto updated = targets.share("alice", 1, binding(), "Renamed");
        QVERIFY(updated); QCOMPARE(updated->id, first->id); QVERIFY(updated->revision != first->revision);
        QVERIFY(!targets.remove("alice", 1, first->id, first->revision));
        QVERIFY(!targets.remove("bob", 1, updated->id, updated->revision));
        QVERIFY(targets.remove("alice", 1, updated->id, updated->revision));
        const auto again = targets.share("alice", 1, binding(), "Board");
        QVERIFY(again); QVERIFY(again->id != first->id); QCOMPARE(changes.size(), 4);
        identities["alice"] = 2;
        QVERIFY(targets.list("alice", 1).isEmpty()); QVERIFY(targets.list("alice", 2).isEmpty());
        QVERIFY(!targets.share("alice", 1, binding(), "Old UI"));
        QVERIFY(!targets.remove("alice", 2, again->id, again->revision));
        targets.revoke("alice"); QVERIFY(targets.bindings().isEmpty());
    }
    void allBindingComponentsPreventStaleInvalidation_data()
    {
        QTest::addColumn<int>("field");
        for (int field = 0; field < 7; ++field) QTest::newRow(qPrintable(QString::number(field))) << field;
    }
    void allBindingComponentsPreventStaleInvalidation()
    {
        QFETCH(int, field);
        SerialFileTargets targets([](const QString&) { return 1; });
        const auto old = binding(); auto current = old;
        if (field == 0) current.connection.session = SessionId{"other"};
        if (field == 1) current.connection.attachment = AttachmentId{"other"};
        if (field == 2) current.connection.epoch = Epoch{"other"};
        if (field == 3) current.connection.generation = Generation{2};
        if (field == 4) current.connection.targetFingerprint = "other";
        if (field == 5) current.ioGeneration = 2;
        if (field == 6) current.configDigest = "other";
        QVERIFY(SerialFileTargets::sameBinding(old, old));
        QVERIFY(!SerialFileTargets::sameBinding(old, current));
        const auto oldGrant = targets.share("alice", 1, old, "Old"); QVERIFY(oldGrant);
        const auto newGrant = targets.share("alice", 1, current, "Current"); QVERIFY(newGrant);
        QVERIFY(oldGrant->id != newGrant->id);
        targets.invalidateBinding(old);
        const auto rows = targets.list("alice", 1); QCOMPARE(rows.size(), 1); QCOMPARE(rows[0].id, newGrant->id);
        targets.invalidateBinding(old); QCOMPARE(targets.list("alice", 1).size(), 1);
        targets.invalidateSession(current.connection.session.str()); QVERIFY(targets.list("alice", 1).isEmpty());
    }
    void incompleteBindingsAndInvalidTitlesFailClosed()
    {
        SerialFileTargets targets([](const QString&) { return 1; });
        for (int field = 0; field < 6; ++field) {
            auto invalid = binding();
            if (field == 0) invalid.connection.session = SessionId{};
            if (field == 1) invalid.connection.attachment = AttachmentId{};
            if (field == 2) invalid.connection.epoch = Epoch{};
            if (field == 3) invalid.connection.generation = Generation{};
            if (field == 4) invalid.ioGeneration = 0;
            if (field == 5) invalid.configDigest.clear();
            QVERIFY(!targets.share("alice", 1, invalid, "Board"));
        }
        for (const auto& title : QStringList{"", "line\nfeed", QString(QChar(127)), QString(QChar(0x85)),
                QString(257, 'a'), QString(86, QChar(0x4e00)), QString(QChar(0xd800))})
            QVERIFY(!targets.share("alice", 1, binding(), title));
        QVERIFY(!targets.share("", 1, binding(), "Board"));
        QVERIFY(!targets.share("alice", 0, binding(), "Board"));
        QVERIFY(!targets.share("alice", 2, binding(), "Board"));
        QVERIFY(targets.share("alice", 1, binding(), QString(256, 'a')));
        QVERIFY(targets.share("alice", 1, binding(), QString(85, QChar(0x4e00))));
    }
    void budgetsPermitUpdatesAndReclaimAcrossClients()
    {
        SerialFileTargets targets([](const QString&) { return 1; });
        for (int client = 0; client < 4; ++client)
            for (int row = 0; row < 8; ++row)
                QVERIFY(targets.share(QString::number(client), 1, binding(QString::number(row)), "Board"));
        QVERIFY(!targets.share("0", 1, binding("ninth"), "Board"));
        QVERIFY(!targets.share("extra", 1, binding(), "Board"));
        QVERIFY(targets.share("0", 1, binding("0"), "Updated"));
        targets.invalidateSession("0");
        QCOMPARE(targets.list("0", 1).size(), 7);
        QVERIFY(targets.share("0", 1, binding("ninth"), "Board"));
        QVERIFY(targets.share("extra", 1, binding(), "Board"));
        targets.revoke("0"); QVERIFY(targets.list("0", 1).isEmpty());
        QCOMPARE(targets.list("1", 1).size(), 7);
        targets.revoke(); QVERIFY(targets.bindings().isEmpty());
    }
};
QTEST_GUILESS_MAIN(SerialFileTargetsTests)
#include "SerialFileTargetsTests.moc"
