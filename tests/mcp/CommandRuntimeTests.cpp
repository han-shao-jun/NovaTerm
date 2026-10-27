#include "mcp/tasks/CommandRuntime.h"
#include "capabilities/terminal/CommandCapabilities.h"
#include "capabilities/McpCapabilityAdapter.h"

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
    NovaTerm::Capabilities::Terminal::CommandCapabilities* capabilities{};
    CommandRuntime runtime{sessions, targets, "instance", CommandRuntime::Delivery{
        [this](const TaskId& task, std::optional<CommandResult> result) {
            if (capabilities) capabilities->complete(task, std::move(result));
            else results.append(std::move(result));
        },
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
    void detachedConfirmationKeepsSessionBusyUntilSettled_data()
    {
        QTest::addColumn<bool>("script"); QTest::addColumn<int>("settlement");
        for (bool script : {false, true})
            for (int settlement = 0; settlement < 5; ++settlement)
                QTest::newRow(qPrintable(QString::number(script) + "-" + QString::number(settlement)))
                    << script << settlement;
    }
    void detachedConfirmationKeepsSessionBusyUntilSettled()
    {
        QFETCH(bool, script); QFETCH(int, settlement);
        Fixture f;
        auto call = script ? f.script() : f.command();
        if (!script) call.request = RunCommandRequest{SessionId{"session"}, Epoch{"epoch"}, "rm -rf /tmp/test"};
        call.formElicitation = true; call.detachedConfirmation = true;
        f.runtime.start(call);
        QVERIFY(f.prompt); QVERIFY(f.prompt->detached);
        QCOMPARE(f.sessions.submissions, 0); QCOMPARE(f.sessions.publications, 0);
        QVERIFY(f.targets.markers.isEmpty());
        QVERIFY(!f.runtime.busy(SessionId{"other"}));
        QVERIFY(f.runtime.busy(SessionId{"session"}));
        if (settlement == 0) {
            f.now = f.prompt->transaction.expires;
            // Expiry is effective immediately, even before the periodic sweep.
        } else if (settlement == 1) f.runtime.revoke("client", "session");
        else if (settlement == 2) f.runtime.disconnect("peer");
        else {
            call.task = TaskId{"answer"}; call.confirmationSupplied = true;
            call.confirmation = f.prompt->transaction; call.answer = ConfirmationAnswer::Decline;
            if (settlement == 4) call.answer = ConfirmationAnswer::Cancel;
            f.runtime.start(call);
        }
        QVERIFY(!f.runtime.busy(SessionId{"session"}));
        QCOMPARE(f.sessions.submissions, 0); QCOMPARE(f.sessions.publications, 0);
    }
    void closeBusyFactsCoverConfirmationUploadAndExecution()
    {
        Fixture f;
        QVERIFY(!f.runtime.busy(SessionId{"session"}));
        QVERIFY(!f.runtime.busy(SessionId{"other"}));
        f.runtime.start(f.script());
        QVERIFY(f.prompt);
        QVERIFY(f.runtime.busy(SessionId{"session"}));
        QVERIFY(!f.runtime.busy(SessionId{"other"}));
        f.runtime.confirm(TaskId{"task"}, ConfirmationAnswer::Accept);
        QCOMPARE(f.sessions.publications, 1);
        QVERIFY(f.runtime.busy(SessionId{"session"}));
        f.runtime.cancel(TaskId{"task"});
        QVERIFY(f.runtime.busy(SessionId{"session"}));
        ScriptPublicationResult publication;
        publication.requestId = f.sessions.published; publication.success = false;
        publication.error = ErrorCode::Cancelled;
        f.runtime.publicationFinished(publication);
        QVERIFY(!f.runtime.busy(SessionId{"session"}));
        Fixture command; command.runtime.start(command.command());
        QCOMPARE(command.sessions.submissions, 1);
        QVERIFY(command.runtime.busy(SessionId{"session"}));
        CommandExecutionResult result;
        result.requestId = command.sessions.submitted;
        result.executionMayHaveStarted = result.terminationConfirmed = true;
        result.outcome = CommandExecutionOutcome::Completed;
        command.runtime.executionFinished(result);
        QVERIFY(!command.runtime.busy(SessionId{"session"}));
    }
    void typedExecutionPortsShareRuntimeAndRejectRevocation()
    {
        namespace C = NovaTerm::Capabilities;
        Fixture f;
        auto registry = C::CapabilityRegistry::terminalCapabilities();
        bool allowed = true;
        C::CapabilityAuthorization auth{[&allowed](const C::PrincipalId&, C::EntryKind,
            const C::ResourceBinding& resource, const C::Definition&) -> std::optional<C::AuthorizationSnapshot> {
            if (!allowed) return std::nullopt;
            return C::AuthorizationSnapshot{resource, 5};
        }};
        C::CapabilityDispatcher dispatcher{registry, auth};
        C::Terminal::CommandCapabilities ports{dispatcher, f.runtime};
        f.capabilities = &ports;
        C::McpCapabilityAdapter local{auth, C::PrincipalId{"client"}};
        C::McpCapabilityAdapter mcp{auth, C::PrincipalId{"client"}};
        C::SessionBinding binding{C::SessionId{"session"}, C::AttachmentId{"attach"}, C::Epoch{"epoch"}, 1, "target"};
        f.sessions.synchronous = true;
        int responses = 0;
        for (int i = 0; i < 2; ++i) {
            f.now += 5001; // Preserve the existing target cooldown for both entrypoints.
            f.runtime.sweep();
            auto call = f.command();
            call.task = TaskId{QString::number(i)};
            call.client = ClientId{"forged"};
            const auto context = i == 0
                ? local.context(binding, 5, C::OperationId{QString::number(i)}, C::Clock::now() + std::chrono::seconds(5))
                : mcp.context(binding, 5, C::OperationId{QString::number(i)}, C::Clock::now() + std::chrono::seconds(5));
            ports.command.invoke(std::move(call), context, [&](auto result) {
                QVERIFY(std::holds_alternative<std::optional<CommandResult>>(result));
                const auto& command = std::get<std::optional<CommandResult>>(result);
                QVERIFY2(command && !command->error, command && command->error ? errorCodeName(command->error->code) : "missing result");
                ++responses;
            });
        }
        QCOMPARE(responses, 2);
        QCOMPARE(f.sessions.submissions, 2);
        allowed = false;
        local.invoke(ports.command, f.command(), binding, 5, C::OperationId{"revoked"},
            C::Clock::now() + std::chrono::seconds(5), [](auto result) {
                QVERIFY(std::holds_alternative<C::Rejection>(result));
            });
        QCOMPARE(f.sessions.submissions, 2);
        f.capabilities = nullptr;
    }
    void disablingLocalEntryBlocksDeferredCommandSubmission_data()
    {
        QTest::addColumn<bool>("duringPublication");
        QTest::newRow("confirmation") << false;
        QTest::newRow("publication") << true;
    }
    void disablingLocalEntryBlocksDeferredCommandSubmission()
    {
        QFETCH(bool, duringPublication);
        namespace C = NovaTerm::Capabilities;
        Fixture f;
        auto registry = C::CapabilityRegistry::terminalCapabilities();
        C::CapabilityAuthorization auth{[](const C::PrincipalId&, C::EntryKind,
            const C::ResourceBinding& resource, const C::Definition&) {
            return std::optional<C::AuthorizationSnapshot>{{resource, 5}};
        }};
        C::CapabilityDispatcher dispatcher{registry, auth};
        C::Terminal::CommandCapabilities ports{dispatcher, f.runtime}; f.capabilities = &ports;
        C::McpCapabilityAdapter local{auth, C::PrincipalId{"client"}};
        C::SessionBinding binding{C::SessionId{"session"}, C::AttachmentId{"attach"}, C::Epoch{"epoch"}, 1, "target"};
        auto call = duringPublication ? f.script() : f.command();
        if (!duringPublication) call.request = RunCommandRequest{SessionId{"session"}, Epoch{"epoch"}, "rm -rf /tmp/test"};
        call.formElicitation = true;
        local.invoke(duringPublication ? ports.script : ports.command, std::move(call), binding, 5, C::OperationId{"op"},
            C::Clock::now() + std::chrono::seconds(5), [](auto) {});
        QVERIFY(f.prompt);
        if (duringPublication) {
            f.runtime.confirm(TaskId{"task"}, ConfirmationAnswer::Accept);
            QCOMPARE(f.sessions.publications, 1);
        }
        local.setEnabled(false);
        if (duringPublication)
            f.runtime.publicationFinished({f.sessions.published, true, {}, "/tmp/test.sh",
                QCryptographicHash::hash(QByteArray{"echo hello\n"}, QCryptographicHash::Sha256)});
        else f.runtime.confirm(TaskId{"task"}, ConfirmationAnswer::Accept);
        QCOMPARE(f.sessions.submissions, 0);
        f.capabilities = nullptr;
    }
    void commandPortDestructionCancelsDeferredSubmission()
    {
        namespace C = NovaTerm::Capabilities;
        Fixture f;
        auto registry = C::CapabilityRegistry::terminalCapabilities();
        C::CapabilityAuthorization auth{[](const C::PrincipalId&, C::EntryKind,
            const C::ResourceBinding& resource, const C::Definition&) { return std::optional<C::AuthorizationSnapshot>{{resource, 5}}; }};
        C::CapabilityDispatcher dispatcher{registry, auth};
        C::McpCapabilityAdapter local{auth, C::PrincipalId{"client"}};
        C::SessionBinding binding{C::SessionId{"session"}, C::AttachmentId{"attach"}, C::Epoch{"epoch"}, 1, "target"};
        auto ports = std::make_unique<C::Terminal::CommandCapabilities>(dispatcher, f.runtime);
        f.capabilities = ports.get();
        auto call = f.command(); call.formElicitation = true;
        call.request = RunCommandRequest{SessionId{"session"}, Epoch{"epoch"}, "rm -rf /tmp/test"};
        local.invoke(ports->command, std::move(call), binding, 5, C::OperationId{"op"},
            C::Clock::now() + std::chrono::seconds(5), [](auto) { QFAIL("destroyed port delivered"); });
        QVERIFY(f.prompt);
        ports.reset(); f.capabilities = nullptr;
        f.runtime.confirm(TaskId{"task"}, ConfirmationAnswer::Accept);
        QCOMPARE(f.sessions.submissions, 0);
        QCOMPARE(dispatcher.activeCount(), size_t(0));
    }
    void cancelledCapabilityCommandsReturnAdmissionBudget()
    {
        namespace C = NovaTerm::Capabilities;
        Fixture f;
        auto registry = C::CapabilityRegistry::terminalCapabilities();
        C::CapabilityAuthorization auth{[](const C::PrincipalId&, C::EntryKind, const C::ResourceBinding& resource,
            const C::Definition&) { return std::optional<C::AuthorizationSnapshot>{{resource, 5}}; }};
        C::CapabilityDispatcher dispatcher{registry, auth};
        C::Terminal::CommandCapabilities ports{dispatcher, f.runtime}; f.capabilities = &ports;
        C::McpCapabilityAdapter local{auth, C::PrincipalId{"client"}};
        C::SessionBinding binding{C::SessionId{"session"}, C::AttachmentId{"attach"}, C::Epoch{"epoch"}, 1, "target"};
        for (size_t i = 0; i <= C::CapabilityDispatcher::MaxOperations; ++i) {
            auto call = f.command(); call.task = TaskId{QString::number(i)}; call.formElicitation = true;
            call.request = RunCommandRequest{SessionId{"session"}, Epoch{"epoch"}, "rm -rf /tmp/test"};
            const C::OperationId operation{QString::number(i)};
            local.invoke(ports.command, std::move(call), binding, 5, operation, C::Clock::now() + std::chrono::seconds(5),
                [](auto) { QFAIL("cancelled request must not deliver a reply"); });
            QVERIFY(dispatcher.cancel(C::PrincipalId{"client"}, operation));
            QCOMPARE(dispatcher.activeCount(), size_t(0));
        }
        QCOMPARE(f.sessions.submissions, 0);
        for (const auto& peer : {QString("peer-one"), QString("peer-two")}) {
            auto call = f.command(); call.task = TaskId{peer}; call.connection = PeerId{peer}; call.formElicitation = true;
            call.request = RunCommandRequest{SessionId{"session"}, Epoch{"epoch"}, "rm -rf /tmp/test"};
            local.invoke(ports.command, std::move(call), binding, 5, C::OperationId{peer}, C::Clock::now() + std::chrono::seconds(5),
                [](auto) { QFAIL("disconnected request delivered"); });
        }
        QCOMPARE(dispatcher.activeCount(), size_t(2));
        ports.disconnect("peer-one");
        QCOMPARE(dispatcher.activeCount(), size_t(1));
        ports.stop();
        QCOMPARE(dispatcher.activeCount(), size_t(0));
        f.capabilities = nullptr;
    }
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
