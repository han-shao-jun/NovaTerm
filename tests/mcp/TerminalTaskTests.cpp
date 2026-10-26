#include "mcp/TerminalTasks.h"
#include "mcp/TerminalInteraction.h"
#include "mcp/ObservationService.h"
#include "mcp/McpAccess.h"
#include "mcp/McpProtocol.h"
#include "mcp/LogReadAdapter.h"
#include "capabilities/McpCapabilityAdapter.h"
#include "session/SessionDirectory.h"
#include "session/SessionTerminalController.h"
#include "TestGrantSupport.h"
#include <QTemporaryDir>
#include <QTest>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QDir>

using namespace NovaTerm::Mcp;
namespace C = NovaTerm::Capabilities;
namespace {
class TaskTransport final : public ITransport {
public:
    bool connectToHost() override { online = true; emit connected(); return true; }
    void disconnect() override { online = false; emit disconnected(); }
    void write(const QByteArray& data) override { written += data; }
    void resizeTerminal(int, int) override {}
    bool isConnected() const override { return online; }
    QString errorString() const override { return {}; }
    bool online{false};
    QByteArray written;
};
struct Fixture {
    QTemporaryDir root{QDir::currentPath() + "/novaterm-task-XXXXXX"};
    TerminalCore core{80, 24};
    TaskTransport transport;
    TerminalSession session{&core};
    SessionDirectory directory;
    AccessStore access{root.path(), std::make_unique<MemoryCredentialStore>()};
    QString client;
    C::CapabilityRegistry registry{C::CapabilityRegistry::terminalCapabilities()};
    C::CapabilityAuthorization auth{[this](const C::PrincipalId&, C::EntryKind,
        const C::ResourceBinding& resource, const C::Definition&) {
        return std::optional<C::AuthorizationSnapshot>{{resource, access.version(client)}};
    }};
    C::CapabilityDispatcher dispatcher{registry, auth};
    ObservationService observations{[] { return clearTicketClock(); }};
    TerminalInteraction interaction{&access, &directory, &observations, dispatcher, nullptr};
    TerminalTasks tasks{access, directory, interaction, "fixture", QByteArray(32, 'k')};
    Fixture() {
        session.attach(&transport, TerminalSession::Ownership::Borrowed, TransportKind::Serial);
        static_cast<void>(session.start()); directory.add(&session);
        if (!root.isValid()) qFatal("Task test directory is unavailable: %s", qPrintable(root.errorString()));
        if (!access.setEnabled(true)) qFatal("Task test AccessStore fixture is unavailable.");
        client = access.addClient("tasks");
        if (client.isEmpty() || !TestSupport::setGrant(access, client, entry(), true)
            || !TestSupport::setTerminalGrant(access, client, entry(), true))
            qFatal("Task test terminal grants are unavailable.");
    }
    SessionDirectory::Entry entry() { return directory.entries().first(); }
    C::InvocationContext context(const QString& request) {
        const auto e = entry();
        C::SessionBinding binding{C::SessionId{e.id}, C::AttachmentId{e.attachmentId},
            C::Epoch{e.epoch}, e.generation, e.targetFingerprint};
        return auth.context(auth.trust(C::PrincipalId{client}, C::EntryKind::Mcp), binding,
            access.version(client), C::OperationId{request}, C::Clock::now() + std::chrono::seconds(10));
    }
    QJsonObject observe() {
        QJsonObject result; const auto e = entry();
        ReadContextRequest request; request.session = NovaTerm::Mcp::SessionId{e.id}; request.epoch = Epoch{e.epoch};
        QElapsedTimer clock; clock.start();
        do {
            result = {};
            interaction.call(client, newId(), ToolRequest{request}, [&result](auto r) { result = r; }, {}, context(newId()));
            while (result.isEmpty() && clock.elapsed() < 3000) QTest::qWait(1);
            if (result.value("error").toObject().value("code") != "BUSY") break;
            QTest::qWait(1);
        } while (clock.elapsed() < 3000);
        if (!result.value("ok").toBool()) qFatal("Task observation fixture failed: %s", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
        return result.value("data").toObject();
    }
    StartTerminalTaskRequest start(const QString& text = "work") {
        const auto e = entry();
        return {NovaTerm::Mcp::SessionId{e.id}, Epoch{e.epoch}, observe().value("inputToken").toString(),
            OperationId{newId()}, {TextAction{text}, LineEndingAction{}}, std::chrono::milliseconds(600000)};
    }
    QJsonObject call(const TerminalTaskRequest& request) {
        QJsonObject result; const auto key = newId();
        tasks.call(client, key, request, [&result](auto r) { result = r; }, context(key));
        QElapsedTimer clock; clock.start();
        while (result.isEmpty() && clock.elapsed() < 3000) QTest::qWait(1);
        return result;
    }
    QueryTerminalTaskRequest query(const QString& task) {
        const auto e = entry();
        return {NovaTerm::Mcp::SessionId{e.id}, Epoch{e.epoch}, TaskId{task}, {}, {}};
    }
};
}
class TerminalTaskTests final : public QObject {
    Q_OBJECT
private slots:
    void exactRetryNeverInputsTwiceAndOperationLookupSurvivesRpcCancel() {
        Fixture f; auto request = f.start(); QVERIFY(!request.inputToken.isEmpty());
        const auto first = f.call(request); QVERIFY2(first.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(first).toJson())));
        const auto task = first.value("data").toObject().value("taskId").toString(); QVERIFY(!task.isEmpty());
        QTRY_COMPARE(f.transport.written, QByteArray("work\r"));
        f.tasks.cancel("disconnected-rpc");
        const auto retry = f.call(request); QVERIFY(retry.value("ok").toBool());
        QCOMPARE(retry.value("data").toObject().value("taskId").toString(), task);
        QTest::qWait(20); QCOMPARE(f.transport.written, QByteArray("work\r"));
        auto query = f.query(task); query.task.reset(); query.operation = request.operation;
        QCOMPARE(f.call(query).value("data").toObject().value("taskId").toString(), task);
        request.actions = {TextAction{"different"}};
        QCOMPARE(f.call(request).value("error").toObject().value("code").toString(), QString("INPUT_SEQUENCE_CONFLICT"));
    }
    void serialSilenceIsRunningWithoutExitEvidence() {
        Fixture f; const auto result = f.call(f.start()); QVERIFY2(result.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(result).toJson())));
        QTRY_COMPARE(f.transport.written, QByteArray("work\r"));
        const auto value = f.call(f.query(result.value("data").toObject().value("taskId").toString())).value("data").toObject();
        QCOMPARE(value.value("status").toString(), QString("running"));
        QVERIFY(value.value("exitCode").isNull()); QVERIFY(!value.value("terminationConfirmed").toBool());
        QVERIFY(!value.value("completed").toBool());
    }
    void humanTakeoverMakesOldStopWriteNothing() {
        Fixture f; auto start = f.start(); const auto result = f.call(start); QVERIFY(result.value("ok").toBool());
        QTRY_COMPARE(f.transport.written, QByteArray("work\r"));
        f.session.writeUserInput("human-command\r"); const auto before = f.transport.written;
        const auto e = f.entry();
        StopTerminalTaskRequest stop{NovaTerm::Mcp::SessionId{e.id}, Epoch{e.epoch},
            TaskId{result.value("data").toObject().value("taskId").toString()}, OperationId{newId()}};
        const auto response = f.call(stop);
        QVERIFY(!response.value("ok").toBool());
        QTest::qWait(20); QCOMPARE(f.transport.written, before);
    }
    void deadlineUsesControllerUnknownAndNeverSendsInterrupt() {
        Fixture f; auto request = f.start(); request.timeout = std::chrono::milliseconds(10);
        const auto result = f.call(request); QVERIFY(result.value("ok").toBool());
        QTRY_COMPARE(f.transport.written, QByteArray("work\r"));
        QTest::qWait(30);
        const auto value = f.call(f.query(result.value("data").toObject().value("taskId").toString())).value("data").toObject();
        QCOMPARE(value.value("status").toString(), QString("unknown"));
        QVERIFY(value.value("exitCode").isNull()); QVERIFY(!value.value("terminationConfirmed").toBool());
        QVERIFY(f.session.terminalController()->owner().isEmpty()); QCOMPARE(f.transport.written, QByteArray("work\r"));
    }
    void cancelledStartDeliveryRetainsAcceptedOperation() {
        Fixture f; auto request = f.start(); int delivered = 0;
        const auto connection = QObject::connect(&f.session, &TerminalSession::terminalControlChanged,
            &f.tasks, [&f] { if (f.session.terminalController()->pending()) f.tasks.cancel("dropped"); });
        f.tasks.call(f.client, "dropped", request, [&delivered](auto) { ++delivered; }, f.context("dropped"));
        QObject::disconnect(connection);
        QTRY_COMPARE(f.transport.written, QByteArray("work\r")); QCOMPARE(delivered, 0);
        auto query = f.query({}); query.task.reset(); query.operation = request.operation;
        const auto result = f.call(query); QVERIFY(result.value("ok").toBool());
        QVERIFY(result.value("data").toObject().value("accepted").toBool());
        QVERIFY(f.call(request).value("ok").toBool());
        QTest::qWait(20); QCOMPARE(f.transport.written, QByteArray("work\r"));
    }
    void repeatedStopInputsOneCtrlCWithoutClaimingTermination() {
        Fixture f; const auto first = f.call(f.start()); QVERIFY(first.value("ok").toBool());
        QTRY_COMPARE(f.transport.written, QByteArray("work\r"));
        const auto e = f.entry();
        StopTerminalTaskRequest stop{NovaTerm::Mcp::SessionId{e.id}, Epoch{e.epoch},
            TaskId{first.value("data").toObject().value("taskId").toString()}, OperationId{newId()}};
        const auto result = f.call(stop); QVERIFY2(result.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(result).toJson())));
        QTRY_COMPARE(f.transport.written, QByteArray("work\r\x03"));
        QVERIFY(f.call(stop).value("ok").toBool());
        QTest::qWait(20); QCOMPARE(f.transport.written, QByteArray("work\r\x03"));
        const auto data = result.value("data").toObject();
        QVERIFY(data.value("stopRequested").toBool()); QVERIFY(!data.value("terminationConfirmed").toBool());
    }
    void commandCompletionBetweenStopSubmissionAndEncodingPreventsCtrlC() {
        Fixture f; auto* ctrl = f.session.terminalController();
        ctrl->handleEvent({InteractiveStreamEventKind::PromptReady, 1, {}, 0});
        auto start = f.start(); start.actions = {CommandAction{"work"}};
        const auto first = f.call(start); QVERIFY(first.value("ok").toBool());
        QTRY_COMPARE(f.transport.written, QByteArray("work\r"));
        const auto e = f.entry();
        StopTerminalTaskRequest stop{NovaTerm::Mcp::SessionId{e.id}, Epoch{e.epoch},
            TaskId{first.value("data").toObject().value("taskId").toString()}, OperationId{newId()}};
        bool injected = false;
        const auto connection = QObject::connect(&f.session, &TerminalSession::terminalControlChanged, &f.tasks,
            [&ctrl, &stop, &injected] {
                if (!injected && ctrl->pending() && ctrl->operation().value("id").toString() == stop.operation.str()) {
                    injected = true;
                    ctrl->handleEvent({InteractiveStreamEventKind::CommandFinished, 0, 0, 0});
                }
            });
        static_cast<void>(f.call(stop)); QObject::disconnect(connection); QVERIFY(injected);
        QTest::qWait(20); QCOMPARE(f.transport.written, QByteArray("work\r"));
        const auto data = f.call(f.query(stop.task.str())).value("data").toObject();
        QCOMPARE(data.value("status").toString(), QString("completed"));
        QCOMPARE(data.value("exitCode").toInt(-1), 0); QVERIFY(data.value("terminationConfirmed").toBool());
    }
    void taskBudgetRejectsBeforeAnyExtraInput() {
        Fixture f;
        for (int i = 0; i < Budget::MaxTerminalTasksPerClient; ++i) {
            const auto result = f.call(f.start()); QVERIFY(result.value("ok").toBool());
            QTRY_COMPARE(f.transport.written.size(), qsizetype((i + 1) * 5));
        }
        const auto before = f.transport.written;
        const auto extra = f.call(f.start());
        QCOMPARE(extra.value("error").toObject().value("code").toString(), QString("BUSY"));
        QTest::qWait(20); QCOMPARE(f.transport.written, before);
    }
    void revokedAndDifferentClientsCannotQueryOldFacts() {
        Fixture f; const auto started = f.call(f.start()); QVERIFY(started.value("ok").toBool());
        const auto id = started.value("data").toObject().value("taskId").toString();
        QTRY_COMPARE(f.transport.written, QByteArray("work\r"));
        const auto original = f.client; f.client = f.access.addClient("other");
        QVERIFY(TestSupport::setGrant(f.access, f.client, f.entry(), true));
        QCOMPARE(f.call(f.query(id)).value("error").toObject().value("code").toString(), QString("TASK_NOT_AVAILABLE"));
        f.client = original; QVERIFY(TestSupport::setGrant(f.access, original, f.entry(), false));
        QVERIFY(!f.call(f.query(id)).value("ok").toBool());
        QCOMPARE(f.transport.written, QByteArray("work\r"));
    }
    void cacheClearPreservesTaskFactsAndReportsLogInvalidation() {
        Fixture f; const auto started = f.call(f.start()); QVERIFY(started.value("ok").toBool());
        QTRY_COMPARE(f.transport.written, QByteArray("work\r"));
        const auto data = started.value("data").toObject();
        auto query = f.query(data.value("taskId").toString());
        query.sinceToken = data.value("baseline").toObject().value("nextToken").toString();
        QVERIFY(!query.sinceToken->isEmpty());
        const auto generation = f.core.cacheGeneration(); QVERIFY(f.core.clearScrollback());
        QTRY_VERIFY(f.core.cacheGeneration() != generation);
        const auto result = f.call(query); QVERIFY(result.value("ok").toBool());
        const auto facts = result.value("data").toObject();
        QCOMPARE(facts.value("taskId").toString(), data.value("taskId").toString());
        QCOMPARE(facts.value("status").toString(), QString("running"));
        QCOMPARE(facts.value("output").toObject().value("status").toString(), QString("invalidated"));
        QCOMPARE(f.transport.written, QByteArray("work\r"));
    }
    void trustedResetInvalidatesRawTaskStop_data() {
        QTest::addColumn<int>("eventKind"); QTest::addColumn<bool>("whileEncoding");
        QTest::newRow("shell-reset-before-stop") << int(InteractiveStreamEventKind::ShellReset) << false;
        QTest::newRow("framing-error-before-stop") << int(InteractiveStreamEventKind::FramingError) << false;
        QTest::newRow("shell-reset-during-encoding") << int(InteractiveStreamEventKind::ShellReset) << true;
        QTest::newRow("framing-error-during-encoding") << int(InteractiveStreamEventKind::FramingError) << true;
    }
    void trustedResetInvalidatesRawTaskStop() {
        QFETCH(int, eventKind); QFETCH(bool, whileEncoding);
        Fixture f; const auto started = f.call(f.start()); QVERIFY(started.value("ok").toBool());
        QTRY_COMPARE(f.transport.written, QByteArray("work\r"));
        auto* ctrl = f.session.terminalController(); const auto oldControl = ctrl->controlGeneration();
        const auto e = f.entry();
        StopTerminalTaskRequest stop{NovaTerm::Mcp::SessionId{e.id}, Epoch{e.epoch},
            TaskId{started.value("data").toObject().value("taskId").toString()}, OperationId{newId()}};
        bool injected = false;
        const auto reset = [&] {
            injected = true;
            ctrl->handleEvent({static_cast<InteractiveStreamEventKind>(eventKind), 0, {}, 0});
        };
        if (!whileEncoding) reset();
        const auto connection = QObject::connect(&f.session, &TerminalSession::terminalControlChanged, &f.tasks, [&] {
            if (whileEncoding && !injected && ctrl->pending()
                && ctrl->operation().value("id").toString() == stop.operation.str()) reset();
        });
        static_cast<void>(f.call(stop)); QObject::disconnect(connection); QVERIFY(injected);
        QTest::qWait(20); QCOMPARE(f.transport.written, QByteArray("work\r"));
        QVERIFY(ctrl->controlGeneration() != oldControl);
        const auto data = f.call(f.query(stop.task.str())).value("data").toObject();
        QCOMPARE(data.value("status").toString(), QString("unknown"));
        QVERIFY(data.value("exitCode").isNull()); QVERIFY(!data.value("terminationConfirmed").toBool());
    }
    void freshTaskCannotAttachToOrdinaryInteractionReplay() {
        Fixture f; const auto request = f.start();
        InteractTerminalRequest input{request.session, request.epoch, request.inputToken,
            request.operation, request.actions, std::chrono::milliseconds(0), {}, {}, {}, false};
        QJsonObject delivered;
        f.interaction.call(f.client, "ordinary", ToolRequest{input}, [&delivered](auto value) { delivered = value; }, {}, f.context("ordinary"));
        QTRY_COMPARE(f.transport.written, QByteArray("work\r"));
        QTRY_VERIFY(!delivered.isEmpty()); QVERIFY(delivered.value("ok").toBool());
        const auto task = f.call(request); QVERIFY(!task.value("ok").toBool());
        QCOMPARE(f.transport.written, QByteArray("work\r"));
    }
    void freshObservationCannotTurnOrdinaryReplayIntoNewTask_data() {
        QTest::addColumn<bool>("completedCommand");
        QTest::newRow("raw-input") << false;
        QTest::newRow("completed-command") << true;
    }
    void freshObservationCannotTurnOrdinaryReplayIntoNewTask() {
        QFETCH(bool, completedCommand);
        Fixture f; auto request = f.start();
        auto* controller = f.session.terminalController();
        if (completedCommand) {
            f.session.commandCoordinator()->notePromptReady(1, controller->userGeneration());
            QVERIFY(controller->readyForCommand());
            request.actions = {CommandAction{"work"}};
            request.inputToken = f.observe().value("inputToken").toString();
        }
        InteractTerminalRequest input{request.session, request.epoch, request.inputToken,
            request.operation, request.actions, std::chrono::milliseconds(0), {}, {}, {}, false};
        QJsonObject delivered;
        f.interaction.call(f.client, "ordinary-fresh", ToolRequest{input},
            [&delivered](auto value) { delivered = std::move(value); }, {}, f.context("ordinary-fresh"));
        QTRY_VERIFY(!delivered.isEmpty()); QVERIFY(delivered.value("ok").toBool());
        QTRY_COMPARE(f.transport.written, QByteArray("work\r"));
        if (completedCommand) {
            QVERIFY(controller->commandActive());
            controller->handleEvent({InteractiveStreamEventKind::CommandFinished, 0, 7, 0});
            QCOMPARE(controller->observation(f.client).value("command").toObject()
                .value("state").toString(), QString("finished"));
        }
        // 最新票据可以用于新意图，但不能将普通输入的旧操作当成新任务。
        const auto oldToken = request.inputToken;
        request.inputToken = f.observe().value("inputToken").toString();
        QVERIFY(request.inputToken != oldToken);
        const auto task = f.call(request);
        QVERIFY2(!task.value("ok").toBool(), QJsonDocument(task).toJson(QJsonDocument::Compact).constData());
        QCOMPARE(task.value("error").toObject().value("code").toString(), QString("INPUT_SEQUENCE_CONFLICT"));
        QCOMPARE(f.transport.written, QByteArray("work\r"));
        // 普通交互自身的精确重试仍兼容，不能因新任务拒绝而破坏它。
        delivered = {};
        f.interaction.call(f.client, "ordinary-retry", ToolRequest{input},
            [&delivered](auto value) { delivered = std::move(value); }, {}, f.context("ordinary-retry"));
        QTRY_VERIFY(!delivered.isEmpty()); QVERIFY(delivered.value("ok").toBool());
        QCOMPARE(f.transport.written, QByteArray("work\r"));
        if (completedCommand)
            f.session.commandCoordinator()->notePromptReady(2, controller->userGeneration());
        request.inputToken = f.observe().value("inputToken").toString();
        request.operation = OperationId{newId()};
        const auto newTask = f.call(request);
        QVERIFY(newTask.value("ok").toBool());
        QVERIFY(newTask.value("data").toObject().value("accepted").toBool());
        QTRY_COMPARE(f.transport.written, QByteArray("work\rwork\r"));
    }
};
QTEST_GUILESS_MAIN(TerminalTaskTests)
#include "TerminalTaskTests.moc"
