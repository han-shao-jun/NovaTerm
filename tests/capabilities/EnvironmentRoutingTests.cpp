#include "capabilities/SessionExecutionRouter.h"
#include "session/EnvironmentTracker.h"
#include <QtTest>
using namespace NovaTerm::Capabilities;
using NovaTerm::Session::EnvironmentTracker;
namespace {
SessionBinding binding() { return {SessionId{"session"}, AttachmentId{"attach"}, Epoch{"epoch"}, 1, {}}; }
RecipeRequirements recipe() { return {CapabilityId{"test.recipe"}, EnvironmentKind::Linux, ExecutionRoute::SharedTerminal, "digest"}; }
RecipeAuthorization authorization() { return {PrincipalId{"client"}, true, false}; }
TargetFacts facts(EnvironmentSnapshot environment) { return {TargetTransport::Serial, environment, false, true, false, false, false}; }
}
class EnvironmentRoutingTests : public QObject
{
    Q_OBJECT
private slots:
    void invalidationPreservesConnectionBinding_data()
    {
        QTest::addColumn<bool>("hasFingerprint");
        QTest::newRow("unverified-target") << false;
        QTest::newRow("verified-target") << true;
    }
    void invalidationPreservesConnectionBinding()
    {
        QFETCH(bool, hasFingerprint);
        auto connection = binding();
        if (hasFingerprint) connection.targetFingerprint = QString("verified-fingerprint");
        EnvironmentTracker tracker;
        tracker.newConnection(connection);
        tracker.confirmOnce(EnvironmentKind::Linux, authorization().principal, recipe().id, recipe().planDigest);
        const auto before = tracker.snapshot();
        QVERIFY(before.ready);
        tracker.invalidate();
        const auto after = tracker.snapshot();
        QVERIFY(sameResource(after.binding, connection));
        QCOMPARE(after.revision, before.revision + 1);
        QCOMPARE(after.kind, EnvironmentKind::Unknown);
        QCOMPARE(after.source, EnvironmentSource::None);
        QVERIFY(after.observedAt == TimePoint{});
        QVERIFY(after.validUntil == TimePoint{});
        QVERIFY(after.readinessEvidenceId.isEmpty());
        QVERIFY(!after.ready);
        QVERIFY(!after.oneSubmission);
        QVERIFY(after.consentPrincipal.empty());
        QVERIFY(after.consentRecipe.empty());
        QVERIFY(after.planDigest.isEmpty());
        tracker.invalidate();
        const auto repeated = tracker.snapshot();
        QVERIFY(sameResource(repeated.binding, connection));
        QCOMPARE(repeated.revision, after.revision + 1);
    }
    void unknownEnvironmentSendsNoProbe()
    {
        EnvironmentTracker tracker;
        tracker.newConnection(binding());
        const auto route = SessionExecutionRouter::resolve(facts(tracker.snapshot()), recipe(), authorization(), Clock::now());
        QCOMPARE(route.reason, RouteUnavailable::UnknownEnvironment);
        QVERIFY(!route.route);
    }
    void plainBootTextCannotAuthorizeLinux()
    {
        EnvironmentTracker tracker;
        tracker.newConnection(binding());
        tracker.observeCandidate(EnvironmentKind::Linux);
        QCOMPARE(tracker.snapshot().kind, EnvironmentKind::Unknown);
        QVERIFY(!SessionExecutionRouter::resolve(facts(tracker.snapshot()), recipe(), authorization(), Clock::now()).route);
    }
    void uartBootToLinuxInvalidatesRecipe()
    {
        EnvironmentTracker tracker;
        tracker.newConnection(binding());
        tracker.confirmOnce(EnvironmentKind::Linux, authorization().principal, recipe().id, recipe().planDigest);
        const auto prior = tracker.snapshot();
        QVERIFY(SessionExecutionRouter::resolve(facts(prior), recipe(), authorization(), Clock::now()).route);
        tracker.observeOutput("U-");
        tracker.observeOutput("Boot");
        QVERIFY(tracker.snapshot().revision > prior.revision);
        QCOMPARE(tracker.snapshot().kind, EnvironmentKind::Unknown);
        QVERIFY(!SessionExecutionRouter::resolve(facts(tracker.snapshot()), recipe(), authorization(), Clock::now()).route);
    }
    void fragmentedRevocationMarkers_data()
    {
        QTest::addColumn<QByteArray>("marker");
        QTest::newRow("bootloader") << QByteArray("U-Boot");
        QTest::newRow("login") << QByteArray("login:");
        QTest::newRow("password") << QByteArray("Password:");
        QTest::newRow("repl") << QByteArray(">>> ");
        QTest::newRow("alternate-screen") << QByteArray("\x1b[?1049h");
    }
    void fragmentedRevocationMarkers()
    {
        QFETCH(QByteArray, marker);
        EnvironmentTracker tracker;
        tracker.newConnection(binding());
        tracker.confirmOnce(EnvironmentKind::Linux, authorization().principal, recipe().id, recipe().planDigest);
        const auto revision = tracker.snapshot().revision;
        for (const char byte : marker) tracker.observeOutput(QByteArrayView(&byte, 1));
        const auto after = tracker.snapshot();
        QCOMPARE(after.kind, EnvironmentKind::Unknown);
        QVERIFY(after.revision > revision);
        QVERIFY(!after.ready);
    }
    void confirmationAllowsOneSubmission()
    {
        EnvironmentTracker tracker;
        tracker.newConnection(binding());
        tracker.declareEnvironment(EnvironmentKind::Linux);
        QVERIFY(!SessionExecutionRouter::resolve(facts(tracker.snapshot()), recipe(), authorization(), Clock::now()).route);
        tracker.confirmOnce(EnvironmentKind::Linux, authorization().principal, recipe().id, recipe().planDigest);
        const auto snapshot = tracker.snapshot();
        QVERIFY(tracker.consumeReadiness(snapshot.revision, snapshot.readinessEvidenceId));
        QVERIFY(!tracker.consumeReadiness(snapshot.revision, snapshot.readinessEvidenceId));
        QVERIFY(!SessionExecutionRouter::resolve(facts(tracker.snapshot()), recipe(), authorization(), Clock::now()).route);
    }
    void unobservedSwitchCannotReuseExpiredEvidence()
    {
        auto now = Clock::now();
        EnvironmentTracker tracker{[&now] { return now; }};
        tracker.newConnection(binding());
        tracker.confirmOnce(EnvironmentKind::Linux, authorization().principal, recipe().id, recipe().planDigest);
        const auto old = tracker.snapshot();
        now += std::chrono::seconds(5);
        QVERIFY(!SessionExecutionRouter::resolve(facts(old), recipe(), authorization(), now).route);
        QCOMPARE(tracker.snapshot().kind, EnvironmentKind::Unknown);
        QVERIFY(tracker.snapshot().revision > old.revision);
    }
    void manualInputInvalidatesRecipe()
    {
        EnvironmentTracker tracker;
        tracker.newConnection(binding());
        tracker.confirmOnce(EnvironmentKind::Linux, authorization().principal, recipe().id, recipe().planDigest);
        const auto revision = tracker.snapshot().revision;
        tracker.invalidate();
        QVERIFY(tracker.snapshot().revision > revision);
        QCOMPARE(tracker.snapshot().kind, EnvironmentKind::Unknown);
    }
    void sharedDiagnosticDoesNotGrantFreeInput()
    {
        EnvironmentTracker tracker;
        tracker.newConnection(binding());
        tracker.confirmOnce(EnvironmentKind::Linux, authorization().principal, recipe().id, recipe().planDigest);
        auto auth = authorization();
        QVERIFY(!auth.fullTerminalInput);
        QVERIFY(SessionExecutionRouter::resolve(facts(tracker.snapshot()), recipe(), auth, Clock::now()).route);
        auth.diagnosticExec = false;
        auth.fullTerminalInput = true;
        QCOMPARE(SessionExecutionRouter::resolve(facts(tracker.snapshot()), recipe(), auth, Clock::now()).reason,
            RouteUnavailable::DiagnosticPermissionRequired);
    }
    void oldAdapterSequenceRejected()
    {
        EnvironmentTracker tracker;
        tracker.newConnection(binding());
        tracker.pairAdapter("adapter");
        AdapterEnvironmentReport report{binding(), "adapter", 2, EnvironmentKind::Linux, "ready", true};
        QVERIFY(tracker.acceptAdapter(report));
        const auto revision = tracker.snapshot().revision;
        report.sequence = 1;
        QVERIFY(!tracker.acceptAdapter(report));
        QCOMPARE(tracker.snapshot().revision, revision);
        report.sequence = 3;
        report.binding.connectionGeneration = 2;
        QVERIFY(!tracker.acceptAdapter(report));
        tracker.newConnection(report.binding);
        QVERIFY(!tracker.acceptAdapter(report));
    }
    void pairedReadinessStillRequiresVisibleOccupancyConsent()
    {
        EnvironmentTracker tracker; tracker.newConnection(binding()); tracker.pairAdapter("adapter");
        QVERIFY(tracker.acceptAdapter({binding(), "adapter", 1, EnvironmentKind::Linux, "ready", true}));
        auto auth = authorization();
        QCOMPARE(SessionExecutionRouter::resolve(facts(tracker.snapshot()), recipe(), auth, Clock::now()).reason,
            RouteUnavailable::ConsentRequired);
        auth.sharedOccupancyConsent = true;
        QVERIFY(SessionExecutionRouter::resolve(facts(tracker.snapshot()), recipe(), auth, Clock::now()).route);
    }
    void fiveTransportMatrixAndBusyBoundaries()
    {
        EnvironmentTracker tracker;
        tracker.newConnection(binding());
        tracker.confirmOnce(EnvironmentKind::Linux, authorization().principal, recipe().id, recipe().planDigest);
        for (auto transport : {TargetTransport::Local, TargetTransport::Ssh, TargetTransport::Serial,
                               TargetTransport::Telnet, TargetTransport::Custom}) {
            auto target = facts(tracker.snapshot());
            target.transport = transport;
            QVERIFY(SessionExecutionRouter::resolve(target, recipe(), authorization(), Clock::now()).route);
            target.tuiOrReplOrLogin = true;
            QVERIFY(!SessionExecutionRouter::resolve(target, recipe(), authorization(), Clock::now()).route);
            target.tuiOrReplOrLogin = false;
            target.fileTransfer = true;
            QVERIFY(!SessionExecutionRouter::resolve(target, recipe(), authorization(), Clock::now()).route);
        }
    }
};
QTEST_GUILESS_MAIN(EnvironmentRoutingTests)
#include "EnvironmentRoutingTests.moc"
