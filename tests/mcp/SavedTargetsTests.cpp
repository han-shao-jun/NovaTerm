#include "mcp/application/SavedTargets.h"
#include "mcp/application/TargetConnections.h"
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
    void closeReservesBeforeQueryAndUsesCurrentEpoch()
    {
        SavedTargets targets; QVERIFY(targets.replace({source()}));
        const auto row = targets.localTargets().first();
        QVERIFY(targets.setShared("alice", 1, row.id, row.configVersion, true));
        QVERIFY(targets.setOpenAllowed("alice", 1, row.id, row.configVersion, true));
        TargetConnections connections(targets);
        TargetConnectionRequest request{row.id, row.configVersion, QUuid::createUuid().toString(QUuid::WithoutBraces)};
        int closes = 0; bool nestedBusy = false;
        connections.setPorts([](const SavedTargetSource&, const TargetConnections::Guard&) {
            return TargetConnectionFacts{"session", "initial", "attachment", "connecting", {}, true, 1};
        }, [&](const QString&, const QString&) {
            nestedBusy = connections.call("alice", 1, request).error == ErrorCode::TargetCloseBusy;
            return TargetConnectionFacts{"session", "discovered", "attachment", "running", {}, false, 1};
        }, [&](const TargetConnectionFacts& original, const QString& epoch, const TargetConnections::Guard&) {
            ++closes;
            auto facts = original; facts.epoch = epoch; facts.state = "closed";
            return TargetConnectionResult{facts, {}};
        });
        QVERIFY(!connections.call("alice", 1, request).error);
        request.action = TargetConnectionAction::Close; request.epoch = "discovered";
        QVERIFY(!connections.call("alice", 1, request).error);
        QVERIFY(nestedBusy); QCOMPARE(closes, 1);
        QVERIFY(!connections.call("alice", 1, request).error); QCOMPARE(closes, 1);
    }
    void closeRechecksRevocationAndCancellationAfterQuery()
    {
        for (bool revoke : {false, true}) {
            SavedTargets targets; QVERIFY(targets.replace({source()}));
            const auto row = targets.localTargets().first();
            QVERIFY(targets.setShared("alice", 1, row.id, row.configVersion, true));
            QVERIFY(targets.setOpenAllowed("alice", 1, row.id, row.configVersion, true));
            TargetConnections connections(targets); bool valid = true; int closes = 0;
            const TargetConnectionFacts facts{"session", "epoch", "attachment", "running", {}, true};
            connections.setPorts([facts](const SavedTargetSource&, const TargetConnections::Guard&) { return facts; },
                [&](const QString&, const QString&) {
                    if (revoke) targets.revoke("alice"); else valid = false;
                    return facts;
                }, [&](const TargetConnectionFacts&, const QString&, const TargetConnections::Guard&) {
                    ++closes; return TargetConnectionResult{facts, {}};
                });
            TargetConnectionRequest request{row.id, row.configVersion, QUuid::createUuid().toString(QUuid::WithoutBraces)};
            QVERIFY(!connections.call("alice", 1, request).error);
            request.action = TargetConnectionAction::Close; request.epoch = "epoch";
            QVERIFY(connections.call("alice", 1, request, [&valid] { return valid; }).error);
            QCOMPARE(closes, 0);
        }
    }
    void closeOnlyOriginalCreatedOperationAndRetryOnce()
    {
        SavedTargets targets; QVERIFY(targets.replace({source()}));
        const auto row = targets.localTargets().first();
        for (const auto& client : QStringList{"alice", "bob"}) {
            QVERIFY(targets.setShared(client, 1, row.id, row.configVersion, true));
            QVERIFY(targets.setOpenAllowed(client, 1, row.id, row.configVersion, true));
        }
        TargetConnections connections(targets);
        TargetConnectionRequest request{row.id, row.configVersion, QUuid::createUuid().toString(QUuid::WithoutBraces)};
        int opens = 0, closes = 0; bool created = false, busy = true;
        connections.setPorts([&](const SavedTargetSource&, const TargetConnections::Guard&) {
            ++opens; return TargetConnectionFacts{"session", "epoch-1", "attachment", "running", {}, created};
        }, {}, [&](const TargetConnectionFacts& facts, const QString&, const TargetConnections::Guard& allowed) {
            ++closes;
            if (busy) return TargetConnectionResult{{}, ErrorCode::TargetCloseBusy};
            const auto nested = connections.call("alice", 1, request);
            if (nested.error != ErrorCode::TargetCloseBusy || !allowed()) ++closes;
            auto closed = facts; closed.state = "closed";
            return TargetConnectionResult{closed, {}};
        });
        QVERIFY(!connections.call("alice", 1, request).error);
        request.action = TargetConnectionAction::Close; request.epoch = "epoch-1";
        QCOMPARE(connections.call("alice", 1, request).error, std::optional{ErrorCode::TargetCloseNotAllowed});
        QCOMPARE(closes, 0);
        request.operationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        request.action = TargetConnectionAction::Open; created = true;
        QVERIFY(!connections.call("alice", 1, request).error);
        request.action = TargetConnectionAction::Close;
        QCOMPARE(connections.call("bob", 1, request).error, std::optional{ErrorCode::TargetOperationNotAvailable});
        auto stale = request; stale.epoch = "epoch-0";
        QCOMPARE(connections.call("alice", 1, stale).error, std::optional{ErrorCode::StaleSessionEpoch});
        QCOMPARE(closes, 0);
        QCOMPARE(connections.call("alice", 1, request).error, std::optional{ErrorCode::TargetCloseBusy});
        busy = false;
        QCOMPARE(connections.call("alice", 1, request).facts.state, QString("closed"));
        QCOMPARE(closes, 2);
        QCOMPARE(connections.call("alice", 1, request).facts.state, QString("closed"));
        QCOMPARE(closes, 2); QCOMPARE(opens, 2);
        QVERIFY(targets.setOpenAllowed("alice", 1, row.id, row.configVersion, false));
        QCOMPARE(connections.call("alice", 1, request).error, std::optional{ErrorCode::TargetOpenPermissionRequired});
    }
    void globalOperationLimitKeepsExistingRecordsQueryable()
    {
        SavedTargets targets; QVERIFY(targets.replace({source()}));
        const auto row = targets.localTargets().first();
        TargetConnections connections(targets); int opens = 0;
        connections.setPorts([&](const SavedTargetSource&, const TargetConnections::Guard&) {
            ++opens; return TargetConnectionFacts{{}, {}, {}, "unavailable", "port_busy", false};
        }, {});
        TargetConnectionRequest first;
        for (int client = 0; client < 9; ++client) {
            const auto id = QString::number(client);
            QVERIFY(targets.setShared(id, 1, row.id, row.configVersion, true));
            QVERIFY(targets.setOpenAllowed(id, 1, row.id, row.configVersion, true));
            for (int i = 0; i < (client < 8 ? 32 : 1); ++i) {
                const TargetConnectionRequest request{row.id, row.configVersion, QUuid::createUuid().toString(QUuid::WithoutBraces)};
                if (client == 0 && i == 0) first = request;
                const auto result = connections.call(id, 1, request);
                if (client < 8) QVERIFY(!result.error);
                else QCOMPARE(result.error, std::optional{ErrorCode::TargetOperationLimit});
            }
        }
        QCOMPARE(opens, 256);
        first.action = TargetConnectionAction::Query; QVERIFY(!connections.call("0", 1, first).error); QCOMPARE(opens, 256);
    }
    void nestedCallbacksRecheckPermissionAndInvocationBeforeConnecting()
    {
        for (int scenario = 0; scenario < 3; ++scenario) {
            SavedTargets targets; QVERIFY(targets.replace({source()}));
            const auto row = targets.localTargets().first();
            QVERIFY(targets.setShared("alice", 1, row.id, row.configVersion, true));
            QVERIFY(targets.setOpenAllowed("alice", 1, row.id, row.configVersion, true));
            TargetConnections connections(targets);
            bool valid = true; int connectionsStarted = 0;
            connections.setPorts([&](const SavedTargetSource&, const TargetConnections::Guard& allowed) {
                if (scenario == 0) static_cast<void>(targets.setOpenAllowed("alice", 1, row.id, row.configVersion, false));
                if (scenario == 1) { auto changed = source(); changed.configurationKey[0] = 'b'; static_cast<void>(targets.replace({changed})); }
                if (scenario == 2) valid = false;
                if (allowed()) ++connectionsStarted;
                return TargetConnectionFacts{{}, {}, {}, "unavailable", "admission_invalidated", false};
            }, {});
            const TargetConnectionRequest request{row.id, row.configVersion, QUuid::createUuid().toString(QUuid::WithoutBraces)};
            QVERIFY(connections.call("alice", 1, request, [&] { return valid; }).error);
            QCOMPARE(connectionsStarted, 0);
        }
    }
    void openingRequiresSeparatePermissionAndRetainsAdmission()
    {
        SavedTargets targets;
        QVERIFY(targets.replace({source()}));
        const auto row = targets.localTargets().first();
        QVERIFY(targets.setShared("alice", 1, row.id, row.configVersion, true));
        TargetConnections connections(targets);
        int opens = 0, queries = 0;
        connections.setPorts([&](const SavedTargetSource&, const TargetConnections::Guard&) {
            ++opens; return TargetConnectionFacts{"session", "epoch-1", "attachment", "connecting", {}, true};
        }, [&](const QString&, const QString&) {
            ++queries; return TargetConnectionFacts{"session", "epoch-2", "attachment", "running", {}, false};
        });
        TargetConnectionRequest request{row.id, row.configVersion, QUuid::createUuid().toString(QUuid::WithoutBraces)};
        QCOMPARE(connections.call("alice", 1, request).error, std::optional{ErrorCode::TargetOpenPermissionRequired});
        QCOMPARE(opens, 0);
        QVERIFY(targets.setOpenAllowed("alice", 1, row.id, row.configVersion, true));
        const auto opened = connections.call("alice", 1, request);
        QVERIFY(!opened.error); QVERIFY(opened.facts.created); QCOMPARE(opens, 1);
        const auto replay = connections.call("alice", 1, request);
        QVERIFY(!replay.error); QVERIFY(replay.facts.created); QCOMPARE(replay.facts.epoch, QString("epoch-2"));
        QCOMPARE(opens, 1);
        request.action = TargetConnectionAction::Query;
        QVERIFY(!connections.call("alice", 1, request).error);
        QCOMPARE(opens, 1); QCOMPARE(queries, 2);
        request.operationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QCOMPARE(connections.call("alice", 1, request).error, std::optional{ErrorCode::TargetOperationNotAvailable});
        QCOMPARE(opens, 1);
        QVERIFY(targets.setShared("alice", 1, row.id, row.configVersion, false));
        QVERIFY(targets.setShared("alice", 1, row.id, row.configVersion, true));
        QVERIFY(!targets.canOpen("alice", 1, row.id));
        QVERIFY(!targets.setOpenAllowed("alice", 2, row.id, row.configVersion, true));
    }
    void connectionOperationsBindArgumentsAndIdentityAndPreserveFailures()
    {
        SavedTargets targets;
        QVERIFY(targets.replace({source()}));
        const auto row = targets.localTargets().first();
        for (const auto& client : QStringList{"alice", "bob"}) {
            QVERIFY(targets.setShared(client, 1, row.id, row.configVersion, true));
            QVERIFY(targets.setOpenAllowed(client, 1, row.id, row.configVersion, true));
        }
        TargetConnections connections(targets);
        int opens = 0;
        connections.setPorts([&](const SavedTargetSource&, const TargetConnections::Guard&) {
            ++opens; return TargetConnectionFacts{{}, {}, {}, "unavailable", "authentication_required", false};
        }, {});
        TargetConnectionRequest request{row.id, row.configVersion, QUuid::createUuid().toString(QUuid::WithoutBraces)};
        QCOMPARE(connections.call("alice", 1, request).facts.reason, QString("authentication_required"));
        QCOMPARE(connections.call("alice", 1, request).facts.reason, QString("authentication_required"));
        QCOMPARE(opens, 1);
        request.action = TargetConnectionAction::Query;
        QCOMPARE(connections.call("bob", 1, request).error, std::optional{ErrorCode::TargetOperationNotAvailable});
        QVERIFY(connections.call("alice", 2, request).error);
        request.configVersion = "changed";
        QVERIFY(connections.call("alice", 1, request).error);
        QCOMPARE(opens, 1);
        auto changed = source(); changed.configurationKey[0] = 'b';
        QVERIFY(targets.replace({changed}));
        const auto current = targets.localTargets().first();
        QVERIFY(targets.setShared("alice", 1, current.id, current.configVersion, true));
        QVERIFY(targets.setOpenAllowed("alice", 1, current.id, current.configVersion, true));
        request.configVersion = current.configVersion;
        request.action = TargetConnectionAction::Open;
        QCOMPARE(connections.call("alice", 1, request).error, std::optional{ErrorCode::TargetOperationConflict});
        QCOMPARE(opens, 1);
    }
    void openingReservationSurvivesReentryAndCapacity()
    {
        SavedTargets targets;
        QVERIFY(targets.replace({source()}));
        const auto row = targets.localTargets().first();
        QVERIFY(targets.setShared("alice", 1, row.id, row.configVersion, true));
        QVERIFY(targets.setOpenAllowed("alice", 1, row.id, row.configVersion, true));
        TargetConnections connections(targets);
        TargetConnectionRequest request{row.id, row.configVersion, QUuid::createUuid().toString(QUuid::WithoutBraces)};
        int opens = 0;
        connections.setPorts([&](const SavedTargetSource&, const TargetConnections::Guard&) {
            ++opens;
            const auto reentry = connections.call("alice", 1, request);
            if (reentry.error || reentry.facts.state != "opening") ++opens;
            return TargetConnectionFacts{{}, {}, {}, "unavailable", "port_busy", false};
        }, {});
        QVERIFY(!connections.call("alice", 1, request).error);
        QCOMPARE(opens, 1);
        const auto first = request;
        for (int i = 1; i < 32; ++i) {
            request.operationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            QVERIFY(!connections.call("alice", 1, request).error);
        }
        request.operationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QCOMPARE(connections.call("alice", 1, request).error, std::optional{ErrorCode::TargetOperationLimit});
        QCOMPARE(opens, 32);
        QVERIFY(!connections.call("alice", 1, first).error);
        QCOMPARE(opens, 32);
    }
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
