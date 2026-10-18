#include "mcp/tasks/CommandRuntime.h"

#include <QCryptographicHash>
#include <QTest>

using namespace NovaTerm::Mcp;

namespace {
class Sessions final : public CommandSessionPort
{
public:
    CommandSessionFacts facts;
    CommandRuntime* runtime{};
    bool synchronous{false};
    int submissions{0}, publications{0}, cancellations{0};
    quint64 submitted{}, published{};
    Sessions()
    {
        facts.reference = {SessionId{"session"}, Epoch{"epoch"}};
        facts.kind = CommandSessionKind::Local;
        facts.target = "target";
        facts.running = facts.readable = facts.commandAllowed = facts.scriptAllowed = true;
        facts.coordinatorAvailable = facts.trustedProfile = facts.promptReady = true;
        facts.executorAvailable = facts.profileAvailable = facts.providerAvailable = true;
        facts.profileVersion = facts.diagnosticPolicyVersion = "profile";
        facts.executor = 1; facts.coordinator = 2; facts.provider = 3;
        facts.promptGeneration = 4; facts.grantVersion = 5;
        facts.diagnostics.insert("system.identity", "uname -srm");
        facts.allowedDiagnostics.insert("system.identity");
    }
    std::optional<CommandSessionFacts> session(const ClientId&, const SessionId&) override
    { return facts; }
    Result<PreparedScript> prepareScript(const CommandSessionFacts&, const RunScriptRequest&) override
    { return PreparedScript{"/tmp/test.sh", "cd -- '/tmp' && sh test.sh", false}; }
    bool submit(quint64, bool, const CommandExecutionRequest& request) override
    {
        ++submissions; submitted = request.requestId;
        if (synchronous) {
            CommandExecutionResult result;
            result.requestId = submitted; result.outcome = CommandExecutionOutcome::Completed;
            result.executionMayHaveStarted = result.terminationConfirmed = true; result.exitCode = 0;
            runtime->executionFinished(result);
        }
        return true;
    }
    void cancel(quint64, bool, quint64, bool) override { ++cancellations; }
    bool publish(quint64, const ScriptPublicationRequest& request) override
    { ++publications; published = request.requestId; return true; }
    void cancelPublication(quint64, quint64) override { ++cancellations; }
};

class Targets final : public CommandTargetStore
{
public:
    QList<TargetProtection> markers;
    Result<std::monostate> reserve(const TargetProtection& marker) override
    { markers.append(marker); return std::monostate{}; }
    bool release(const QString& target, const QString& execution) override
    {
        for (auto it = markers.begin(); it != markers.end(); ++it)
            if (it->target == target && it->execution == execution) { markers.erase(it); return true; }
        return false;
    }
    QList<TargetProtection> protections() const override { return markers; }
    bool acknowledge(const QString& target) override
    { for (const auto& marker : markers) if (marker.target == target) return release(target, marker.execution); return false; }
};

struct Fixture
{
    Sessions sessions;
    Targets targets;
    qint64 now{100};
    QList<std::optional<CommandResult>> results;
    std::optional<CommandPrompt> prompt;
    CommandRuntime runtime{sessions, targets, "instance", CommandRuntime::Delivery{
        [this](const TaskId&, std::optional<CommandResult> result) { results.append(std::move(result)); },
        [](const TaskId&, std::chrono::milliseconds) {},
        [this](const TaskId&, const CommandPrompt& value) { prompt = value; return true; },
        [] {}}, [this] { return now; }};
    Fixture() { sessions.runtime = &runtime; }
    CommandInvocation command()
    {
        CommandInvocation call;
        call.task = TaskId{"task"}; call.client = ClientId{"client"}; call.connection = PeerId{"peer"};
        call.request = RunCommandRequest{SessionId{"session"}, Epoch{"epoch"}, "pwd"};
        return call;
    }
    CommandInvocation script()
    {
        auto call = command();
        call.request = RunScriptRequest{SessionId{"session"}, Epoch{"epoch"}, "echo hello\n", "test.sh", "/tmp", "sh test.sh"};
        call.formElicitation = true;
        call.confirmationDigest = QByteArray(32, 'd');
        return call;
    }
    void approveScript()
    {
        runtime.start(script());
        QVERIFY(prompt);
        runtime.confirm(TaskId{"task"}, ConfirmationAnswer::Accept);
        QCOMPARE(sessions.publications, 1);
    }
};
} // namespace

class CommandRuntimeTests : public QObject
{
    Q_OBJECT
private slots:
    void synchronousCompletionRetainsFinalRecord()
    {
        Fixture f; f.sessions.synchronous = true;
        f.runtime.start(f.command());
        QCOMPARE(f.results.size(), 1); QVERIFY(f.results.first());
        QVERIFY(!f.results.first()->error);
        QCOMPARE(f.runtime.executionRecords().size(), 1);
        QVERIFY(f.runtime.executionRecords().first().complete);
        QVERIFY(f.targets.markers.isEmpty());
    }
    void diagnosticTicketIsValidatedBeforeSubmission()
    {
        Fixture f; auto call = f.command();
        call.request = ExecuteCommandRequest{SessionId{"session"}, Epoch{"epoch"}, DiagnosticId{"system.identity"}, "profile", {}};
        f.runtime.start(call);
        QCOMPARE(f.sessions.submissions, 0);
        QCOMPARE(f.results.last()->error->code, ErrorCode::CommandTicketScopeInvalid);
        call.task = TaskId{"second"};
        call.ticket = DiagnosticTicket{"execution", {SessionId{"session"}, Epoch{"epoch"}}, DiagnosticId{"system.identity"}, "profile", "profile", ClientAccessVersion{5}, 1000};
        f.runtime.start(call);
        QCOMPARE(f.sessions.submissions, 1);
    }
    void cancellationAndUnknownCompletionRetainProtection()
    {
        Fixture f; f.runtime.start(f.command()); f.runtime.cancel(TaskId{"task"});
        QCOMPARE(f.sessions.cancellations, 1);
        f.now += 1000; f.runtime.sweep();
        QVERIFY(f.runtime.executionRecords().first().complete);
        QVERIFY(!f.targets.markers.isEmpty());
        QVERIFY(f.results.isEmpty());
        CommandExecutionResult late;
        late.requestId = f.sessions.submitted; late.executionMayHaveStarted = true; late.terminationConfirmed = true;
        f.runtime.executionFinished(late);
        QVERIFY(f.targets.markers.isEmpty()); QVERIFY(f.results.isEmpty());
    }
    void cancelledUploadLatePublicationNeverInvokes()
    {
        Fixture f; f.approveScript(); f.runtime.cancel(TaskId{"task"});
        QVERIFY(!f.targets.markers.isEmpty());
        ScriptPublicationResult result;
        result.requestId = f.sessions.published; result.success = true;
        result.contentHash = QCryptographicHash::hash("echo hello\n", QCryptographicHash::Sha256);
        result.resolvedTargetPath = "/tmp/test.sh";
        f.runtime.publicationFinished(result);
        QCOMPARE(f.sessions.submissions, 0); QVERIFY(f.targets.markers.isEmpty());
        QVERIFY(f.results.isEmpty());
        QVERIFY(f.runtime.executionRecords().first().result.publication->published);
    }
    void providerDisappearanceRetainsUnknownPublication()
    {
        Fixture f; f.approveScript(); f.runtime.providerUnavailable(3);
        QCOMPARE(f.sessions.submissions, 0); QVERIFY(!f.targets.markers.isEmpty());
        QCOMPARE(f.results.last()->error->code, ErrorCode::ScriptProviderUnavailable);
        QVERIFY(f.runtime.executionRecords().first().result.publication->publicationMayHaveOccurred);
    }
    void revocationSettlesActiveCallWithoutService()
    {
        Fixture f;
        f.runtime.start(f.command());
        f.runtime.revoke("client", "session");
        QCOMPARE(f.sessions.cancellations, 1);
        QCOMPARE(f.results.size(), 1);
        QCOMPARE(f.results.last()->error->code, ErrorCode::SessionNotAvailable);
        QVERIFY(!f.targets.markers.isEmpty());
    }
    void ownerDestructionCancelsAndKeepsProtection_data()
    {
        QTest::addColumn<bool>("script");
        QTest::newRow("execution") << false;
        QTest::newRow("publication") << true;
    }
    void ownerDestructionCancelsAndKeepsProtection()
    {
        QFETCH(bool, script);
        Sessions sessions; Targets targets; int callbacks = 0; qint64 now = 100;
        auto runtime = std::make_unique<CommandRuntime>(sessions, targets, "instance", CommandRuntime::Delivery{
            [&](const TaskId&, std::optional<CommandResult>) { ++callbacks; },
            [](const TaskId&, std::chrono::milliseconds) {},
            [](const TaskId&, const CommandPrompt&) { return true; }, [] {}}, [&] { return now; });
        sessions.runtime = runtime.get();
        Fixture calls;
        runtime->start(script ? calls.script() : calls.command());
        if (script) runtime->confirm(TaskId{"task"}, ConfirmationAnswer::Accept);
        QVERIFY(!targets.markers.isEmpty());
        runtime.reset();
        QCOMPARE(sessions.cancellations, 1);
        QCOMPARE(callbacks, 0);
        QVERIFY(!targets.markers.isEmpty());
    }
    void confirmationExpiresWithInjectedClock()
    {
        Fixture f; f.runtime.start(f.script());
        QVERIFY(f.prompt);
        f.now = f.prompt->transaction.expires;
        f.runtime.confirm(TaskId{"task"}, ConfirmationAnswer::Accept);
        QCOMPARE(f.sessions.publications, 0);
        QCOMPARE(f.results.last()->error->code, ErrorCode::CommandConfirmationStale);
    }
    void successfulPublicationInvokesOnlyAfterRevalidation()
    {
        Fixture f; f.approveScript(); ++f.sessions.facts.grantVersion;
        ScriptPublicationResult result;
        result.requestId = f.sessions.published; result.success = true;
        result.contentHash = QCryptographicHash::hash("echo hello\n", QCryptographicHash::Sha256);
        result.resolvedTargetPath = "/tmp/test.sh";
        f.runtime.publicationFinished(result);
        QCOMPARE(f.sessions.submissions, 0);
        QCOMPARE(f.results.last()->error->code, ErrorCode::CommandConfirmationStale);
        QVERIFY(f.results.last()->publication->published);
    }
};

QTEST_GUILESS_MAIN(CommandRuntimeTests)
#include "CommandRuntimeTests.moc"
