#include "capabilities/DiagnosticTaskManager.h"
#include <QtTest>
using namespace NovaTerm::Capabilities;
namespace {
class FakeSession final : public RecipeSessionPort
{
public:
    TargetFacts target;
    bool permit{true}, consume{true}, submitOk{true}, synchronous{false};
    int submits{0}, cancels{0}, releases{0};
    bool confirmed{false};
    std::vector<StepCompletion> pending;
    TargetFacts facts(const SessionBinding&) override { return target; }
    bool diagnosticAuthorized(const InvocationContext&, const CapabilityId&) override { return permit; }
    bool reserve(const SessionBinding&, const OperationId&) override { return true; }
    bool consumeReadiness(const SessionBinding&, quint64, const QString&) override { return consume; }
    bool submit(ExecutionRoute, const SessionBinding&, const OperationId&, const RecipeStep&, StepCompletion done) override {
        ++submits;
        if (!submitOk) return false;
        if (synchronous) { done({true, true, true, "sync"}); return true; }
        pending.push_back(std::move(done));
        return true;
    }
    void cancel(const SessionBinding&, const OperationId&) override { ++cancels; }
    void settle(const SessionBinding&, const OperationId&, bool terminationConfirmed) override {
        ++releases; confirmed = terminationConfirmed;
    }
};
SessionBinding binding() { return {SessionId{"session"}, AttachmentId{"attach"}, Epoch{"epoch"}, 1, {}}; }
RecipeInvocation invocation() {
    return {{CapabilityId{"test.recipe"}, EnvironmentKind::Linux, ExecutionRoute::Isolated, "digest"},
        {{QByteArray{"uname"}, 65536, std::chrono::seconds(5)}}};
}
struct Harness {
    CapabilityRegistry registry;
    bool authorizationAllowed{true};
    TimePoint now{Clock::now()};
    std::function<void()> duringAuthorization;
    CapabilityAuthorization auth{[this](const PrincipalId&, EntryKind, const ResourceBinding& resource, const Definition&) {
        const auto callback = std::move(duringAuthorization);
        duringAuthorization = {};
        if (callback) callback();
        return authorizationAllowed ? std::optional<AuthorizationSnapshot>{{resource, 1}} : std::nullopt;
    }};
    CapabilityDispatcher dispatcher{registry, auth, [this] { return now; }};
    FakeSession session;
    DiagnosticTaskManager tasks{session, [this] { return now; }};
    Harness() {
        registry.add({CapabilityId{"test.recipe"}, 1, ImplementationStatus::Implemented, ResourceKind::Session, Policy::Recipe, {}});
        session.target.isolatedAvailable = true;
        session.target.environment.binding = binding();
        session.target.environment.kind = EnvironmentKind::Linux;
        session.target.environment.observedAt = now;
        session.target.environment.validUntil = session.target.environment.observedAt + std::chrono::seconds(5);
    }
    InvocationLease admit(QString principal, QString operation) {
        auto trusted = auth.trust(PrincipalId{principal}, EntryKind::LocalAgent);
        auto context = auth.context(trusted, binding(), 1, OperationId{operation}, now + std::chrono::seconds(5));
        auto result = dispatcher.admit(CapabilityId{"test.recipe"}, context);
        return std::move(std::get<InvocationLease>(result));
    }
};
}
class DiagnosticTaskTests : public QObject
{
    Q_OBJECT
private slots:
    void backendFailureDoesNotRetryOtherRoute()
    {
        Harness h;
        h.session.submitOk = false;
        int completed = 0;
        h.tasks.start(invocation(), h.admit("client", "one"), [&](auto result) {
            ++completed; QCOMPARE(result.outcome, DiagnosticOutcome::Failed);
        });
        QCOMPARE(h.session.submits, 1);
        QCOMPARE(completed, 1);
        QCOMPARE(h.tasks.activeCount(), size_t(0));
        QVERIFY(h.session.confirmed);
    }
    void diagnosticBudgetsAndSingleCompletion()
    {
        Harness h;
        int completed = 0;
        for (int i = 0; i < 32; ++i) {
            h.tasks.start(invocation(), h.admit(QString::number(i / 4), QString::number(i)),
                [&](auto) { ++completed; });
        }
        QCOMPARE(h.tasks.activeCount(), size_t(32));
        h.tasks.start(invocation(), h.admit("extra", "overflow"), [&](auto result) {
            ++completed; QCOMPARE(result.outcome, DiagnosticOutcome::Busy);
        });
        QCOMPARE(h.session.submits, 32);
        QVERIFY(!h.session.pending.empty());
        auto done = h.session.pending.front();
        done({true, true, true, "ok"});
        done({true, true, true, "duplicate"});
        QCOMPARE(completed, 2);
        QCOMPARE(h.tasks.activeCount(), size_t(31));
    }
    void principalBudgetAndRevokedStepSendsNothing()
    {
        Harness h;
        int busy = 0;
        for (int i = 0; i < 5; ++i) h.tasks.start(invocation(), h.admit("client", QString::number(i)),
            [&](auto result) { if (result.outcome == DiagnosticOutcome::Busy) ++busy; });
        QCOMPARE(busy, 1);
        QCOMPARE(h.session.submits, 4);
        h.session.permit = false;
        auto multiple = invocation();
        multiple.steps.push_back(multiple.steps.front());
        h.tasks.start(multiple, h.admit("other", "revoked"), [](auto result) {
            QCOMPARE(result.outcome, DiagnosticOutcome::Denied);
        });
        QCOMPARE(h.session.submits, 4);
    }
    void cancelLeaseDoesNotClaimRemoteStopped()
    {
        Harness h;
        int completed = 0;
        h.tasks.start(invocation(), h.admit("client", "op"), [&](auto result) {
            ++completed; QCOMPARE(result.outcome, DiagnosticOutcome::Unknown);
            QVERIFY(result.executionMayHaveStarted);
        });
        QVERIFY(h.tasks.cancel(PrincipalId{"client"}, OperationId{"op"}));
        QCOMPARE(h.session.cancels, 1);
        QCOMPARE(completed, 1);
        QVERIFY(!h.session.confirmed);
        QVERIFY(!h.session.pending.empty());
        h.session.pending.front()({true, true, true, "late"});
        QCOMPARE(completed, 1);
    }
    void revokedOutputSuppressedOnEveryTerminalPath_data()
    {
        QTest::addColumn<int>("terminalPath");
        QTest::newRow("recipe-unknown") << 0;
        QTest::newRow("recipe-cancel") << 1;
        QTest::newRow("authority-dispatcher-cancel") << 2;
        QTest::newRow("authority-expire") << 3;
    }
    void revokedOutputSuppressedOnEveryTerminalPath()
    {
        QFETCH(int, terminalPath);
        Harness h;
        auto request = invocation(); request.steps.push_back(request.steps.front());
        std::optional<DiagnosticResult> delivered;
        h.tasks.start(request, h.admit("client", "op"), [&](auto value) { delivered = std::move(value); });
        QVERIFY(!h.session.pending.empty());
        h.session.pending.front()({true, true, true, "retained-secret"});
        QCOMPARE(h.session.submits, 2);
        if (terminalPath < 2) h.session.permit = false;
        else h.authorizationAllowed = false;
        if (terminalPath == 0) h.session.pending.back()({false, true, false, "new-secret"});
        else if (terminalPath == 1) QVERIFY(h.tasks.cancel(PrincipalId{"client"}, OperationId{"op"}));
        else if (terminalPath == 2) QVERIFY(h.dispatcher.cancel(PrincipalId{"client"}, OperationId{"op"}));
        else { h.now += std::chrono::seconds(6); h.tasks.sweep(); }
        QVERIFY(delivered);
        QCOMPARE(delivered->outcome, DiagnosticOutcome::Denied);
        QVERIFY(delivered->output.isEmpty());
        QVERIFY(!delivered->terminationConfirmed);
        QVERIFY(delivered->executionMayHaveStarted);
        QVERIFY(!h.session.confirmed);
        QCOMPARE(h.tasks.activeCount(), size_t(0));
    }
    void sweepToleratesAuthorizationReentry_data()
    {
        QTest::addColumn<bool>("destroyOwner");
        QTest::newRow("cancel-current-job") << false;
        QTest::newRow("destroy-owner") << true;
    }
    void sweepToleratesAuthorizationReentry()
    {
        QFETCH(bool, destroyOwner);
        Harness h;
        auto tasks = std::make_unique<DiagnosticTaskManager>(h.session, [&h] { return h.now; });
        tasks->start(invocation(), h.admit("client", "op"), [](auto) {});
        h.duringAuthorization = [&] {
            if (destroyOwner) tasks.reset();
            else QVERIFY(tasks->cancel(PrincipalId{"client"}, OperationId{"op"}));
        };
        tasks->sweep();
        if (destroyOwner) QVERIFY(!tasks);
        else QCOMPARE(tasks->activeCount(), size_t(0));
    }
    void synchronousCompletionMayDestroyOwner()
    {
        Harness h;
        h.session.synchronous = true;
        auto tasks = std::make_unique<DiagnosticTaskManager>(h.session);
        bool completed = false;
        tasks->start(invocation(), h.admit("client", "op"), [&](auto result) {
            QCOMPARE(result.outcome, DiagnosticOutcome::Completed);
            completed = true;
            tasks.reset();
        });
        QVERIFY(completed);
        QVERIFY(!tasks);
    }
    void cancellationCompletionMayDestroyOwner()
    {
        Harness h;
        auto tasks = std::make_unique<DiagnosticTaskManager>(h.session);
        tasks->start(invocation(), h.admit("client", "op"), [&](auto result) {
            QCOMPARE(result.outcome, DiagnosticOutcome::Unknown);
            tasks.reset();
        });
        QVERIFY(tasks->cancel(PrincipalId{"client"}, OperationId{"op"}));
        QVERIFY(!tasks);
        QVERIFY(!h.session.confirmed);
    }
    void destroyedManagerDropsLateCallback()
    {
        Harness h;
        bool called = false;
        {
            DiagnosticTaskManager tasks{h.session};
            tasks.start(invocation(), h.admit("client", "op"), [&](auto) { called = true; });
        }
        QVERIFY(!h.session.confirmed);
        QVERIFY(!h.session.pending.empty());
        h.session.pending.front()({true, true, true, "late"});
        QVERIFY(!called);
    }
};
QTEST_GUILESS_MAIN(DiagnosticTaskTests)
#include "DiagnosticTaskTests.moc"
