/** @file McpTests.cpp
 *  @brief MCP 协议、授权、跨客户端摘要及受限命令的离线端到端检查。
 */
#include "mcp/McpService.h"
#include "mcp/McpProtocol.h"
#include "mcp/CommandPolicy.h"
#include "mcp/CommandRiskPolicy.h"
#include "renderer/TerminalRenderer.h"
#include "session/CommandExecutionTypes.h"
#include "session/ISessionCommandExecutor.h"
#include "session/LocalDiagnosticProtocol.h"
#include "session/LocalSessionCommandExecutor.h"
#include "session/SshSessionScriptProvider.h"
#include "session/SessionCommandFacade.h"
#include "session/SessionCommandCoordinator.h"
#include "session/SessionInputArbiter.h"
#include "transport/SshTransport.h"
#include "ui/widgets/McpSettingsDialog.h"
#include "ElaApplication.h"
#include "ElaCheckBox.h"
#include "ElaTreeWidget.h"
#include <QApplication>
#include <QFontDatabase>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QTimer>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>
#include <QtEndian>

#include <algorithm>
#include <tuple>

using namespace NovaTerm::Mcp;
class SshTransportTestAccess
{
public:
    static void knownHosts(SshTransport& transport, const QString& path)
    {
        transport._knownHostsPath = path;
        transport._processUserConfiguration = false;
    }
    static void connected(SshTransport& transport)
    {
        transport._connected = true;
        transport._serverHostKeyFingerprint = QString(64, QLatin1Char('a'));
    }
    static int queued(SshTransport& transport) { return int(transport._commandQueue.size()); }
    static QByteArray queuedCommand(SshTransport& transport)
    {
        return transport._commandQueue.isEmpty() ? QByteArray{} : transport._commandQueue.head().command;
    }
    static void finish(SshTransport& transport, bool confirmed, QByteArray output = "Linux test x86_64\n")
    {
        if (transport._commandQueue.isEmpty()) return;
        const auto request = transport._commandQueue.dequeue();
        CommandExecutionResult result;
        result.requestId = request.requestId;
        result.connectionGeneration = request.generation;
        result.executionMayHaveStarted = true;
        result.terminationConfirmed = confirmed;
        result.outcome = confirmed ? CommandExecutionOutcome::Completed
                                   : CommandExecutionOutcome::Disconnected;
        if (confirmed) result.exitCode = 0;
        result.standardOutput = output;
        emit transport.boundedCommandFinished(result);
    }
};

class LocalFake final : public ITransport
{
public:
    bool connectToHost() override { online = true; emit connected(); return true; }
    void disconnect() override { if (online) { online = false; emit disconnected(); } }
    void write(const QByteArray& bytes) override { written += bytes; }
    void resizeTerminal(int, int) override {}
    bool isConnected() const override { return online; }
    QString errorString() const override { return {}; }
    bool online{false};
    QByteArray written;
};

class DelayedScriptProvider final : public ISessionScriptProvider
{
public:
    [[nodiscard]] bool isAvailable() const override { return true; }
    bool writeScript(const ScriptWriteRequest& request) override
    {
        if (_request.requestId != 0)
            return false;
        _request = request;
        ++writeCount;
        return true;
    }
    void cancelWrite(quint64 requestId) override
    {
        if (_request.requestId == requestId)
            ++cancelCount;
    }

    int writeCount{0};
    int cancelCount{0};

private:
    ScriptWriteRequest _request;
};

class TestCommandExecutor final : public ISessionCommandExecutor
{
public:
    explicit TestCommandExecutor(
        std::shared_ptr<QList<quint64>> cancelledRequests = {},
        QObject* parent = nullptr)
        : ISessionCommandExecutor(parent)
        , _cancelledRequests(std::move(cancelledRequests))
    {
    }

    [[nodiscard]] bool isAvailable() const override { return true; }
    [[nodiscard]] CommandExecutorCapabilities capabilities() const override
    {
        return {CommandExecutionMode::Isolated, true, true, true};
    }
    [[nodiscard]] CommandPlatformProfile profile() const override
    {
        return CommandPlatformProfile::forTransport(TransportKind::LocalShell);
    }
    [[nodiscard]] QString targetFingerprint() const override
    {
        return QString(64, QLatin1Char('b'));
    }
    [[nodiscard]] bool execute(const CommandExecutionRequest& request) override
    {
        ++submissionCount;
        lastRequest = request;
        CommandExecutionResult result;
        result.requestId = request.requestId;
        result.outcome = CommandExecutionOutcome::Completed;
        result.executionMayHaveStarted = true;
        result.terminationConfirmed = true;
        result.exitCode = 0;
        result.standardOutput = QByteArrayLiteral("fixture executor\n");
        QMetaObject::invokeMethod(this, [this, result] { emit finished(result); },
                                  Qt::QueuedConnection);
        return true;
    }
    void cancel(quint64 requestId) override
    {
        cancelledRequest = requestId;
        if (_cancelledRequests)
            _cancelledRequests->append(requestId);
    }

    int submissionCount{0};
    CommandExecutionRequest lastRequest;
    quint64 cancelledRequest{0};

private:
    std::shared_ptr<QList<quint64>> _cancelledRequests;
};

struct PendingExecutorState
{
    int submissions{0};
    QList<quint64> cancelledRequests;
};

class PendingCommandExecutor final : public ISessionCommandExecutor
{
public:
    explicit PendingCommandExecutor(std::shared_ptr<PendingExecutorState> state,
                                    QObject* parent = nullptr)
        : ISessionCommandExecutor(parent)
        , _state(std::move(state))
    {
    }

    [[nodiscard]] bool isAvailable() const override { return true; }
    [[nodiscard]] CommandExecutorCapabilities capabilities() const override
    {
        return {CommandExecutionMode::Isolated, true, true, true};
    }
    [[nodiscard]] CommandPlatformProfile profile() const override
    {
        return CommandPlatformProfile::forTransport(TransportKind::LocalShell);
    }
    [[nodiscard]] QString targetFingerprint() const override
    {
        return QString(64, QLatin1Char('c'));
    }
    [[nodiscard]] bool execute(const CommandExecutionRequest&) override
    {
        ++_state->submissions;
        return true;
    }
    void cancel(quint64 requestId) override
    {
        _state->cancelledRequests.append(requestId);
    }

private:
    std::shared_ptr<PendingExecutorState> _state;
};

class Host
{
public:
    ~Host()
    {
        process.closeWriteChannel();
        QElapsedTimer timer;
        timer.start();
        while (process.state() != QProcess::NotRunning && timer.elapsed() < 1500) {
            QCoreApplication::processEvents();
            process.waitForFinished(10);
        }
        if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(1000); }
    }
    bool start(const QString& runtime, const QByteArray& token,
               const QString& instance = {}, bool humanForm = false,
               const QString& protocolVersion = ProtocolVersion)
    {
        _protocolVersion = protocolVersion;
        _humanForm = humanForm;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("NOVATERM_MCP_RUNTIME_DIR", runtime);
        environment.insert("NOVATERM_MCP_TOKEN", QString::fromLatin1(token));
        process.setProcessEnvironment(environment);
        process.start(QString::fromUtf8(NOVATERM_MCP_BRIDGE), instance.isEmpty()
            ? QStringList{} : QStringList{QStringLiteral("--instance"), instance});
        if (!process.waitForStarted(3000)) return false;
        if (protocolVersion == QStringLiteral("2026-07-28")) {
            send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "server/discover"},
                {"params", QJsonObject{{"_meta", requestMeta()}}}});
            const auto response = next();
            if (!response.value("result").toObject().value("supportedVersions")
                    .toArray().contains(protocolVersion)) return false;
        } else {
            send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                {"params", QJsonObject{{"protocolVersion", protocolVersion},
                    {"capabilities", humanForm
                        ? QJsonObject{{"elicitation", QJsonObject{{"form", QJsonObject{}}}}}
                        : QJsonObject{}},
                    {"clientInfo", QJsonObject{{"name", "NovaTerm fixture"}, {"version", "1"}}}}}});
            const auto response = next();
            if (response.value("result").toObject().value("protocolVersion") != protocolVersion) return false;
            send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
        }
        return true;
    }
    QJsonObject requestMeta() const
    {
        return {{"io.modelcontextprotocol/protocolVersion", _protocolVersion},
            {"io.modelcontextprotocol/clientInfo", QJsonObject{{"name", "NovaTerm fixture"}, {"version", "1"}}},
            {"io.modelcontextprotocol/clientCapabilities", _humanForm
                ? QJsonObject{{"elicitation", QJsonObject{{"form", QJsonObject{}}}}}
                : QJsonObject{}}};
    }
    void setHumanForm(bool supported) { _humanForm = supported; }
    void send(QJsonObject message)
    {
        if (_protocolVersion == QStringLiteral("2026-07-28")
            && message.value("method") == QStringLiteral("tools/call")) {
            auto params = message.value("params").toObject();
            params.insert("_meta", requestMeta());
            message.insert("params", params);
        }
        process.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
        process.waitForBytesWritten(10);
    }
    int begin(const QString& name, const QJsonObject& arguments)
    {
        const int id = ++counter;
        send({{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"},
            {"params", QJsonObject{{"name", name}, {"arguments", arguments}}}});
        return id;
    }
    QJsonObject next(int timeout = 5000)
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeout) {
            QCoreApplication::processEvents();
            buffered += process.readAllStandardOutput();
            const auto newline = buffered.indexOf('\n');
            if (newline >= 0) {
                const auto line = buffered.left(newline);
                buffered.remove(0, newline + 1);
                return QJsonDocument::fromJson(line).object();
            }
            if (process.state() == QProcess::NotRunning) return {};
            process.waitForReadyRead(5);
        }
        return {};
    }
    QJsonObject call(const QString& name, const QJsonObject& arguments = {})
    {
        begin(name, arguments);
        return next().value("result").toObject().value("structuredContent").toObject();
    }
    QProcess process;
    QByteArray buffered;
    int counter{1};
    QString _protocolVersion{QString::fromLatin1(ProtocolVersion)};
    bool _humanForm{false};
};

struct Fixture
{
    QTemporaryDir root;
    TerminalCore core{80, 24};
    LocalFake local;
    std::unique_ptr<SshTransport> ssh;
    TerminalSession session{&core};
    Service service{root.path() + "/state", root.path() + "/instances", std::make_unique<MemoryCredentialStore>()};
    QString client;
    Fixture()
    {
        session.attach(&local, TerminalSession::Ownership::Borrowed, TransportKind::LocalShell);
        static_cast<void>(session.start());
        service.directory().add(&session);
        client = service.access().addClient("fixture");
    }
    void makeSsh()
    {
        SshConfig config;
        config.host = "fixture.invalid";
        config.username = "fixture";
        ssh = std::make_unique<SshTransport>(config);
        SshTransportTestAccess::connected(*ssh);
        session.attach(ssh.get(), TerminalSession::Ownership::Borrowed, TransportKind::Ssh);
        static_cast<void>(service.directory().entries());
    }
    QJsonObject identity()
    {
        const auto entry = service.directory().entries().first();
        return {{"sessionId", entry.id}, {"epoch", entry.epoch}};
    }
    bool enable(bool execute = false)
    {
        QSet<QString> commands;
        if (execute) for (const auto& command : CommandPolicy::catalog()) commands.insert(command.id);
        return !client.isEmpty() && service.access().setEnabled(true)
            && service.access().setGrant(client, service.directory().entries().first(), true, commands);
    }
    QByteArray token() { return service.access().exportToken(client).value_or(QByteArray{}); }
    QString runtime() { return root.path() + "/instances"; }
};

class McpTests final : public QObject
{
    Q_OBJECT
private slots:
    void framingAndAuthentication();
    void rejectsDangerousAndOversizedArguments();
    void nonblockingSnapshotPreservesIndependentProgress();
    void stdioWithoutApplicationAndInitialization();
    void readSearchAndClientIsolation();
    void truncatedOutputNeverAdvancesCursor();
    void revokeAndReconnectInvalidateAccess();
    void commandsRequireSeparateGrantAndAreDeduplicated();
    void unknownExecutionProtectsTarget();
    void privateMarkersSurviveServiceRestart();
    void cancelledCommandNeverReplays();
    void utf8ExpansionIsBounded();
    void settingsDialogSeparatesReadAndScriptPermission();
    void settingsDialogLocalShellFixedDiagnosticsCanBeSelected();
    void stateDirectoryOwnedByTokenDefaultOwnerIsSecurable();
    void duplicateIndexSurvivesEvictionAndReset();
    void protocolLifecycleAndOversizedInput();
    void instanceSelectionAndDisconnectStayExplicit();
    void ccSwitchConfigurationIsSingleServerObject();
    void sessionCommandFacadeRoutesTrustedExecutors();
    void registeredSessionExecutorIsUsedWithoutTransportCast();
    void sessionCommandFacadeCancelsRequestsBeforeRebinding();
    void localDiagnosticHelperAcceptsOnlyFixedCommands();
    void localSessionExecutorKeepsCommandsIsolatedAndBounded();
    void localShellCommandsRunOutsideInteractiveTransport();
    void localFixedDiagnosticsStayIsolatedWithTrustedShellProfile();
    void sessionInvalidationCompletesPendingExecution();
    void publishedSnapshotSwitchDefaultsToEnabled();
    void publishedSnapshotPreservesCaptureTime();
    void commandRiskPolicyDefaultsToHumanConfirmation();
    void scriptRiskPolicyDeniesCredentialAndSecurityTampering();
    void freeCommandSchemaIsBoundedAndStrict();
    void interactiveGrantIsSeparateFromReadAndFixedCommands();
    void riskyCommandRequiresHumanConfirmation();
    void lowRiskCommandUsesCurrentInteractiveTransport();
    void unverifiedSshCommandUsesCurrentTerminal();
    void unverifiedSshRiskyCommandRequiresHumanConfirmation();
    void humanDeclineLeavesTerminalUntouched();
    void humanAcceptRunsOnlyAfterConfirmation();
    void unknownRiskCanBeConfirmed();
    void mrtrRequiresBoundOneShotConfirmation();
    void scriptContentRequiresConfirmationAndNeverEntersTerminal();
    void scriptMrtrConfirmationIsSignedAndOneShot();
    void userTypingCancelsInFlightScriptUploadBeforeInvocation();
    void commandAndScriptProductGrantsAreIndependent();
};

void McpTests::humanDeclineLeavesTerminalUntouched()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Serial;
    runtime.transport.insert(QStringLiteral("interactivePromptPattern"), QStringLiteral("device> $"));
    TerminalSession session(runtime);
    LocalFake transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed, TransportKind::Serial);
    QVERIFY(session.start());
    Service service(root.filePath("state"), root.filePath("instances"),
                    std::make_unique<MemoryCredentialStore>());
    service.directory().add(&session);
    const auto client = service.access().addClient("fixture");
    QVERIFY(service.access().setEnabled(true));
    const auto entry = service.directory().entries().first();
    QVERIFY(service.access().setGrant(client, entry, true, {}, true, true));
    QTRY_COMPARE(service.status(), QStringLiteral("Listening"));
    emit transport.readyRead(QByteArrayLiteral("device> "));
    QTRY_VERIFY(session.commandCoordinator()->isPromptReady());
    Host host;
    QVERIFY(host.start(root.filePath("instances"),
                       service.access().exportToken(client).value_or(QByteArray{}), {}, true));
    const int callId = host.begin("novaterm_run_command",
        QJsonObject{{"sessionId", entry.id}, {"epoch", entry.epoch},
                    {"command", QStringLiteral("rm -rf ./cache")}});
    const auto request = host.next();
    QCOMPARE(request.value("method").toString(),
             QStringLiteral("elicitation/create"));
    const auto promptId = request.value("id");
    QVERIFY(promptId.isString());
    QVERIFY(request.value("params").toObject().value("message")
        .toString().contains(QStringLiteral("rm -rf ./cache")));
    host.send({{"jsonrpc", "2.0"}, {"id", promptId},
               {"result", QJsonObject{{"action", "decline"}}}});
    const auto response = host.next();
    QCOMPARE(response.value("id").toInt(), callId);
    QCOMPARE(response.value("result").toObject()
                 .value("structuredContent").toObject()
                 .value("error").toObject().value("code").toString(),
             QStringLiteral("COMMAND_CONFIRMATION_DECLINED"));
    QVERIFY(transport.written.isEmpty());

    const int cancelledCall = host.begin("novaterm_run_command",
        QJsonObject{{"sessionId", entry.id}, {"epoch", entry.epoch},
                    {"command", QStringLiteral("rm -rf ./cache")}});
    const auto cancelPrompt = host.next();
    QCOMPARE(cancelPrompt.value("method").toString(), QStringLiteral("elicitation/create"));
    host.send({{"jsonrpc", "2.0"}, {"id", cancelPrompt.value("id")},
               {"result", QJsonObject{{"action", "cancel"}}}});
    const auto cancelled = host.next();
    QCOMPARE(cancelled.value("id").toInt(), cancelledCall);
    QCOMPARE(cancelled.value("result").toObject().value("structuredContent")
                 .toObject().value("error").toObject().value("code").toString(),
             QStringLiteral("COMMAND_CONFIRMATION_CANCELLED"));
    QVERIFY(transport.written.isEmpty());

    const quint64 previousPrompt = session.commandCoordinator()->promptGeneration();
    emit transport.readyRead(QByteArrayLiteral("device> "));
    QTRY_VERIFY_WITH_TIMEOUT(session.commandCoordinator()->isPromptReady(), 1000);
    QVERIFY(session.commandCoordinator()->promptGeneration() > previousPrompt);
    const int staleCall = host.begin("novaterm_run_command",
        QJsonObject{{"sessionId", entry.id}, {"epoch", entry.epoch},
                    {"command", QStringLiteral("rm -rf ./cache")}});
    const auto stalePrompt = host.next();
    QCOMPARE(stalePrompt.value("method").toString(), QStringLiteral("elicitation/create"));
    const quint64 confirmedAgainst = session.commandCoordinator()->promptGeneration();
    emit transport.readyRead(QByteArrayLiteral("device> "));
    QTRY_VERIFY_WITH_TIMEOUT(session.commandCoordinator()->promptGeneration() > confirmedAgainst,
                             1000);
    host.send({{"jsonrpc", "2.0"}, {"id", stalePrompt.value("id")},
               {"result", QJsonObject{{"action", "accept"},
                   {"content", QJsonObject{{"confirmed", true}}}}}});
    const auto stale = host.next();
    QCOMPARE(stale.value("id").toInt(), staleCall);
    QCOMPARE(stale.value("result").toObject().value("structuredContent")
                 .toObject().value("error").toObject().value("code").toString(),
             QStringLiteral("COMMAND_CONFIRMATION_STALE"));
    QVERIFY(transport.written.isEmpty());
}

void McpTests::humanAcceptRunsOnlyAfterConfirmation()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Serial;
    runtime.transport.insert(QStringLiteral("interactivePromptPattern"), QStringLiteral("device> $"));
    TerminalSession session(runtime);
    LocalFake transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed, TransportKind::Serial);
    QVERIFY(session.start());
    Service service(root.filePath("state"), root.filePath("instances"),
                    std::make_unique<MemoryCredentialStore>());
    service.directory().add(&session);
    const auto client = service.access().addClient("fixture");
    QVERIFY(service.access().setEnabled(true));
    const auto entry = service.directory().entries().first();
    QVERIFY(service.access().setGrant(client, entry, true));
    QTRY_COMPARE(service.status(), QStringLiteral("Listening"));
    emit transport.readyRead(QByteArrayLiteral("device> "));
    QTRY_VERIFY(session.commandCoordinator()->isPromptReady());
    Host host;
    QVERIFY(host.start(root.filePath("instances"),
                       service.access().exportToken(client).value_or(QByteArray{}), {}, true));
    const int callId = host.begin("novaterm_run_command",
        QJsonObject{{"sessionId", entry.id}, {"epoch", entry.epoch},
                    {"command", QStringLiteral("rm -rf ./cache")}});
    const auto request = host.next();
    QCOMPARE(request.value("method").toString(), QStringLiteral("elicitation/create"));
    const auto promptId = request.value("id");
    QTimer::singleShot(50, &transport, [&transport] {
        emit transport.readyRead(QByteArrayLiteral("rm -rf ./cache\r\ndevice> "));
    });
    host.send({{"jsonrpc", "2.0"}, {"id", promptId}, {"result", QJsonObject{
        {"action", "accept"}, {"content", QJsonObject{{"confirmed", true}}}}}});
    QTRY_COMPARE_WITH_TIMEOUT(transport.written,
                              QByteArrayLiteral("rm -rf ./cache\r"), 1000);
    const auto response = host.next();
    QCOMPARE(response.value("id").toInt(), callId);
    QVERIFY(response.value("result").toObject().value("structuredContent")
        .toObject().value("ok").toBool());

}

void McpTests::unknownRiskCanBeConfirmed()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Serial;
    runtime.transport.insert(QStringLiteral("interactivePromptPattern"),
                             QStringLiteral("device> $"));
    TerminalSession session(runtime);
    LocalFake transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed,
                   TransportKind::Serial);
    QVERIFY(session.start());
    Service service(root.filePath(QStringLiteral("state")),
                    root.filePath(QStringLiteral("instances")),
                    std::make_unique<MemoryCredentialStore>());
    service.directory().add(&session);
    const auto client = service.access().addClient(QStringLiteral("fixture"));
    QVERIFY(service.access().setEnabled(true));
    const auto entry = service.directory().entries().first();
    QVERIFY(service.access().setGrant(client, entry, true));
    QTRY_COMPARE(service.status(), QStringLiteral("Listening"));
    emit transport.readyRead(QByteArrayLiteral("device> "));
    QTRY_VERIFY(session.commandCoordinator()->isPromptReady());
    Host host;
    QVERIFY(host.start(root.filePath(QStringLiteral("instances")),
        service.access().exportToken(client).value_or(QByteArray{}), {}, true));
    const int callId = host.begin("novaterm_run_command",
        QJsonObject{{"sessionId", entry.id}, {"epoch", entry.epoch},
                    {"command", QStringLiteral("unknown-tool --do-work")}});
    const auto prompt = host.next();
    QCOMPARE(prompt.value("method").toString(), QStringLiteral("elicitation/create"));
    QVERIFY(transport.written.isEmpty());
    QTimer::singleShot(50, &transport, [&transport] {
        emit transport.readyRead(QByteArrayLiteral("unknown-tool --do-work\r\ndevice> "));
    });
    host.send({{"jsonrpc", "2.0"}, {"id", prompt.value("id")},
        {"result", QJsonObject{{"action", "accept"},
            {"content", QJsonObject{{"confirmed", true}}}}}});
    QTRY_COMPARE_WITH_TIMEOUT(transport.written,
                              QByteArrayLiteral("unknown-tool --do-work\r"), 1000);
    const auto response = host.next();
    QCOMPARE(response.value("id").toInt(), callId);
    QVERIFY(response.value("result").toObject().value("structuredContent")
        .toObject().value("ok").toBool());
}

void McpTests::mrtrRequiresBoundOneShotConfirmation()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Serial;
    runtime.transport.insert(QStringLiteral("interactivePromptPattern"), QStringLiteral("device> $"));
    TerminalSession session(runtime);
    LocalFake transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed, TransportKind::Serial);
    QVERIFY(session.start());
    Service service(root.filePath("state"), root.filePath("instances"),
                    std::make_unique<MemoryCredentialStore>());
    service.directory().add(&session);
    const auto client = service.access().addClient("fixture");
    QVERIFY(service.access().setEnabled(true));
    const auto entry = service.directory().entries().first();
    QVERIFY(service.access().setGrant(client, entry, true, {}, true, true));
    QTRY_COMPARE(service.status(), QStringLiteral("Listening"));
    emit transport.readyRead(QByteArrayLiteral("device> "));
    QTRY_VERIFY(session.commandCoordinator()->isPromptReady());
    Host host;
    QVERIFY(host.start(root.filePath("instances"),
        service.access().exportToken(client).value_or(QByteArray{}), {}, true,
        QStringLiteral("2026-07-28")));

    const QJsonObject arguments{{"sessionId", entry.id}, {"epoch", entry.epoch},
                                {"command", QStringLiteral("rm -rf ./cache")}};
    const int firstCall = host.begin("novaterm_run_command", arguments);
    const auto needed = host.next();
    QCOMPARE(needed.value("id").toInt(), firstCall);
    const auto inputRequired = needed.value("result").toObject();
    QCOMPARE(inputRequired.value("resultType").toString(), QStringLiteral("input_required"));
    const auto requestState = inputRequired.value("requestState").toString();
    QVERIFY(!requestState.isEmpty());
    QVERIFY(inputRequired.value("inputRequests").toObject().contains(QStringLiteral("confirm")));
    QVERIFY(transport.written.isEmpty());

    const int concurrentId = host.begin("novaterm_run_command", arguments);
    const auto concurrent = host.next();
    QCOMPARE(concurrent.value("id").toInt(), concurrentId);
    QCOMPARE(concurrent.value("result").toObject().value("structuredContent")
        .toObject().value("error").toObject().value("code").toString(),
        QStringLiteral("BUSY"));

    QString forgedState = requestState;
    const QChar finalCharacter = forgedState.back();
    forgedState[forgedState.size() - 1] = finalCharacter == QLatin1Char('0')
        ? QLatin1Char('1') : QLatin1Char('0');
    const int forgedId = ++host.counter;
    host.send({{"jsonrpc", "2.0"}, {"id", forgedId}, {"method", "tools/call"},
        {"params", QJsonObject{{"name", "novaterm_run_command"}, {"arguments", arguments},
            {"requestState", forgedState}, {"inputResponses", QJsonObject{{"confirm",
                QJsonObject{{"action", "accept"}, {"content", QJsonObject{{"confirmed", true}}}}}}}}}});
    const auto forged = host.next();
    QCOMPARE(forged.value("id").toInt(), forgedId);
    QCOMPARE(forged.value("result").toObject().value("structuredContent")
        .toObject().value("error").toObject().value("code").toString(),
        QStringLiteral("COMMAND_CONFIRMATION_STALE"));
    QVERIFY(transport.written.isEmpty());

    QTimer::singleShot(50, &transport, [&transport] {
        emit transport.readyRead(QByteArrayLiteral("rm -rf ./cache\r\ndevice> "));
    });
    host.setHumanForm(false);
    const int retryId = ++host.counter;
    host.send({{"jsonrpc", "2.0"}, {"id", retryId}, {"method", "tools/call"},
        {"params", QJsonObject{{"name", "novaterm_run_command"}, {"arguments", arguments},
            {"requestState", requestState}, {"inputResponses", QJsonObject{{"confirm",
                QJsonObject{{"action", "accept"}, {"content", QJsonObject{{"confirmed", true}}}}}}}}}});
    const auto completed = host.next();
    QCOMPARE(completed.value("id").toInt(), retryId);
    QCOMPARE(completed.value("result").toObject().value("resultType").toString(),
             QStringLiteral("complete"));
    const auto completedPayload = completed.value("result").toObject()
        .value("structuredContent").toObject();
    QVERIFY2(completedPayload.value("ok").toBool(),
        qPrintable(completedPayload.value("error").toObject().value("code").toString()));
    QCOMPARE(transport.written, QByteArrayLiteral("rm -rf ./cache\r"));

    const int replayId = ++host.counter;
    host.send({{"jsonrpc", "2.0"}, {"id", replayId}, {"method", "tools/call"},
        {"params", QJsonObject{{"name", "novaterm_run_command"}, {"arguments", arguments},
            {"requestState", requestState}, {"inputResponses", QJsonObject{{"confirm",
                QJsonObject{{"action", "accept"}, {"content", QJsonObject{{"confirmed", true}}}}}}}}}});
    const auto replay = host.next();
    QCOMPARE(replay.value("id").toInt(), replayId);
    QCOMPARE(replay.value("result").toObject().value("structuredContent")
        .toObject().value("error").toObject().value("code").toString(),
        QStringLiteral("COMMAND_CONFIRMATION_STALE"));
    QCOMPARE(transport.written, QByteArrayLiteral("rm -rf ./cache\r"));
}

void McpTests::scriptContentRequiresConfirmationAndNeverEntersTerminal()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString workingDirectory = root.path();
    const QString targetPath = root.filePath(QStringLiteral("build_script.sh"));
    const QByteArray previousContent = QByteArrayLiteral("old confirmed content\n");
    QFile existing(targetPath);
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QCOMPARE(existing.write(previousContent), qint64(previousContent.size()));
    existing.close();

    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::LocalShell;
    runtime.transport.insert(QStringLiteral("interactiveShellKind"), QStringLiteral("posix"));
    TerminalSession session(runtime);
    LocalFake transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed,
                   TransportKind::LocalShell);
    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    Service service(root.filePath(QStringLiteral("state")),
                    root.filePath(QStringLiteral("instances")),
                    std::make_unique<MemoryCredentialStore>());
    service.directory().add(&session);
    const auto client = service.access().addClient(QStringLiteral("fixture"));
    QVERIFY(service.access().setEnabled(true));
    const auto entry = service.directory().entries().first();
    QVERIFY(service.access().setGrant(client, entry, true, {}, false, false, true));
    QTRY_COMPARE(service.status(), QStringLiteral("Listening"));
    emit transport.readyRead(QByteArrayLiteral("\x1b]633;NT;PROMPT;1;0\x07"));
    QTRY_VERIFY(session.commandCoordinator()->isPromptReady());
    Host host;
    QVERIFY(host.start(root.filePath(QStringLiteral("instances")),
        service.access().exportToken(client).value_or(QByteArray{}), {}, true));

    const QByteArray script = QByteArrayLiteral(
        "#!/bin/sh\nprintf 'SCRIPT_BODY_MUST_NOT_ENTER_TERMINAL'\n");
    const QJsonObject arguments{{"sessionId", entry.id}, {"epoch", entry.epoch},
        {"scriptContent", QString::fromUtf8(script)}, {"targetPath", targetPath},
        {"workingDirectory", workingDirectory},
        {"invocation", QStringLiteral("sh ./build_script.sh")}};
    Host hostWithoutConfirmation;
    QVERIFY(hostWithoutConfirmation.start(root.filePath(QStringLiteral("instances")),
        service.access().exportToken(client).value_or(QByteArray{})));
    const auto noCapability = hostWithoutConfirmation.call(
        "novaterm_run_script", arguments);
    QCOMPARE(noCapability.value("error").toObject().value("code").toString(),
             QStringLiteral("CLIENT_CONFIRMATION_UNAVAILABLE"));
    QFile afterUnavailable(targetPath);
    QVERIFY(afterUnavailable.open(QIODevice::ReadOnly));
    QCOMPARE(afterUnavailable.readAll(), previousContent);
    afterUnavailable.close();
    QVERIFY(transport.written.isEmpty());

    const int declineId = host.begin("novaterm_run_script", arguments);
    const auto declinePrompt = host.next();
    QCOMPARE(declinePrompt.value("method").toString(), QStringLiteral("elicitation/create"));
    QVERIFY(declinePrompt.value("params").toObject().value("message")
        .toString().contains(targetPath));
    host.send({{"jsonrpc", "2.0"}, {"id", declinePrompt.value("id")},
        {"result", QJsonObject{{"action", "decline"}}}});
    const auto declined = host.next();
    QCOMPARE(declined.value("id").toInt(), declineId);
    QCOMPARE(declined.value("result").toObject().value("structuredContent")
        .toObject().value("error").toObject().value("code").toString(),
        QStringLiteral("COMMAND_CONFIRMATION_DECLINED"));
    QFile checkOld(targetPath);
    QVERIFY(checkOld.open(QIODevice::ReadOnly));
    QCOMPARE(checkOld.readAll(), previousContent);
    checkOld.close();
    QVERIFY(transport.written.isEmpty());

    const QString missingTarget = root.filePath(
        QStringLiteral("missing-directory/never-created.sh"));
    auto failedArguments = arguments;
    failedArguments.insert(QStringLiteral("targetPath"), missingTarget);
    const int writeFailureId = host.begin("novaterm_run_script", failedArguments);
    const auto writeFailurePrompt = host.next();
    QCOMPARE(writeFailurePrompt.value("method").toString(),
             QStringLiteral("elicitation/create"));
    host.send({{"jsonrpc", "2.0"}, {"id", writeFailurePrompt.value("id")},
        {"result", QJsonObject{{"action", "accept"},
            {"content", QJsonObject{{"confirmed", true}}}}}});
    const auto writeFailure = host.next();
    QCOMPARE(writeFailure.value("id").toInt(), writeFailureId);
    QCOMPARE(writeFailure.value("result").toObject().value("structuredContent")
        .toObject().value("error").toObject().value("code").toString(),
        QStringLiteral("SCRIPT_WRITE_FAILED"));
    QVERIFY(transport.written.isEmpty());
    QVERIFY(!QFile::exists(missingTarget));

    const int acceptId = host.begin("novaterm_run_script", arguments);
    const auto acceptPrompt = host.next();
    QCOMPARE(acceptPrompt.value("method").toString(), QStringLiteral("elicitation/create"));
    QTimer::singleShot(100, &transport, [&transport] {
        emit transport.readyRead(QByteArrayLiteral(
            "sh ./build_script.sh\r\nSCRIPT_OUTPUT_OK\r\n"
            "\x1b]633;NT;PROMPT;2;0\x07"));
    });
    host.send({{"jsonrpc", "2.0"}, {"id", acceptPrompt.value("id")},
        {"result", QJsonObject{{"action", "accept"},
            {"content", QJsonObject{{"confirmed", true}}}}}});
    const auto accepted = host.next();
    QCOMPARE(accepted.value("id").toInt(), acceptId);
    const auto payload = accepted.value("result").toObject()
        .value("structuredContent").toObject();
    QVERIFY2(payload.value("ok").toBool(),
        qPrintable(payload.value("error").toObject().value("code").toString()));
    QFile finalFile(targetPath);
    QVERIFY(finalFile.open(QIODevice::ReadOnly));
    QCOMPARE(finalFile.readAll(), script);
    QVERIFY(transport.written.contains("sh ./build_script.sh"));
    QVERIFY(!transport.written.contains("SCRIPT_BODY_MUST_NOT_ENTER_TERMINAL"));
    QVERIFY(payload.value("data").toObject().value("stdout")
        .toString().contains(QStringLiteral("SCRIPT_OUTPUT_OK")));
}

void McpTests::scriptRiskPolicyDeniesCredentialAndSecurityTampering()
{
    CommandRiskPolicy policy;
    const auto classify = [&policy](QByteArrayView script) {
        return policy.classifyScript(script, QStringLiteral("/tmp/generated.sh"),
            QStringLiteral("/tmp"), QStringLiteral("sh generated.sh")).decision;
    };
    QCOMPARE(classify(QByteArrayView("cat ~/.aws/credentials")), RiskDecision::Deny);
    QCOMPARE(classify(QByteArrayView("cmdkey /list")), RiskDecision::Deny);
    QCOMPARE(classify(QByteArrayView("cat /proc/self/environ")), RiskDecision::Deny);
    QCOMPARE(classify(QByteArrayView("systemctl stop firewalld")), RiskDecision::Deny);
    QCOMPARE(classify(QByteArrayView("mkfs.ext4 /dev/sda")), RiskDecision::Deny);
    QCOMPARE(classify(QByteArrayView("#!/bin/sh\nprintf safe\n")), RiskDecision::Confirm);
}

void McpTests::scriptMrtrConfirmationIsSignedAndOneShot()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::LocalShell;
    runtime.transport.insert(QStringLiteral("interactiveShellKind"), QStringLiteral("posix"));
    TerminalSession session(runtime);
    LocalFake transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed,
                   TransportKind::LocalShell);
    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    Service service(root.filePath(QStringLiteral("state")),
                    root.filePath(QStringLiteral("instances")),
                    std::make_unique<MemoryCredentialStore>());
    service.directory().add(&session);
    const auto client = service.access().addClient(QStringLiteral("fixture"));
    QVERIFY(service.access().setEnabled(true));
    const auto entry = service.directory().entries().first();
    QVERIFY(service.access().setGrant(client, entry, true, {}, true, false, true));
    QTRY_COMPARE(service.status(), QStringLiteral("Listening"));
    emit transport.readyRead(QByteArrayLiteral("\x1b]633;NT;PROMPT;1;0\x07"));
    QTRY_VERIFY(session.commandCoordinator()->isPromptReady());
    Host host;
    QVERIFY(host.start(root.filePath(QStringLiteral("instances")),
        service.access().exportToken(client).value_or(QByteArray{}), {}, true,
        QStringLiteral("2026-07-28")));

    const QString targetPath = root.filePath(QStringLiteral("mrtr_script.sh"));
    const QString body = QStringLiteral("#!/bin/sh\nprintf 'MRTR_BODY_PRIVATE'\n");
    const QJsonObject arguments{{"sessionId", entry.id}, {"epoch", entry.epoch},
        {"scriptContent", body}, {"targetPath", targetPath},
        {"workingDirectory", root.path()},
        {"invocation", QStringLiteral("sh ./mrtr_script.sh")}};
    const int initialId = host.begin("novaterm_run_script", arguments);
    const auto required = host.next();
    QCOMPARE(required.value("id").toInt(), initialId);
    const auto result = required.value("result").toObject();
    QCOMPARE(result.value("resultType").toString(), QStringLiteral("input_required"));
    QString requestState = result.value("requestState").toString();
    QVERIFY(!requestState.isEmpty());
    QVERIFY(result.value("inputRequests").toObject().contains(QStringLiteral("confirm")));
    QVERIFY(!QFile::exists(targetPath));
    QVERIFY(transport.written.isEmpty());

    QVERIFY(service.access().setGrant(client, entry, true, {}, true, false, false));
    const int revokedRetryId = ++host.counter;
    host.send({{"jsonrpc", "2.0"}, {"id", revokedRetryId}, {"method", "tools/call"},
        {"params", QJsonObject{{"name", "novaterm_run_script"},
            {"arguments", arguments}, {"requestState", requestState},
            {"inputResponses", QJsonObject{{"confirm", QJsonObject{{"action", "accept"},
                {"content", QJsonObject{{"confirmed", true}}}}}}}}}});
    const auto revoked = host.next();
    QCOMPARE(revoked.value("result").toObject().value("structuredContent")
        .toObject().value("error").toObject().value("code").toString(),
        QStringLiteral("COMMAND_CONFIRMATION_STALE"));
    QVERIFY(!QFile::exists(targetPath));
    QVERIFY(transport.written.isEmpty());

    QVERIFY(service.access().setGrant(client, entry, true, {}, true, false, true));
    const int reissuedId = host.begin("novaterm_run_script", arguments);
    const auto reissued = host.next();
    QCOMPARE(reissued.value("id").toInt(), reissuedId);
    QCOMPARE(reissued.value("result").toObject().value("resultType").toString(),
             QStringLiteral("input_required"));
    requestState = reissued.value("result").toObject().value("requestState").toString();
    QVERIFY(!requestState.isEmpty());

    QString forgedState = requestState;
    forgedState[forgedState.size() - 1] = forgedState.back() == QLatin1Char('0')
        ? QLatin1Char('1') : QLatin1Char('0');
    const int forgedId = ++host.counter;
    host.send({{"jsonrpc", "2.0"}, {"id", forgedId}, {"method", "tools/call"},
        {"params", QJsonObject{{"name", "novaterm_run_script"},
            {"arguments", arguments}, {"requestState", forgedState},
            {"inputResponses", QJsonObject{{"confirm", QJsonObject{{"action", "accept"},
                {"content", QJsonObject{{"confirmed", true}}}}}}}}}});
    const auto forged = host.next();
    QCOMPARE(forged.value("result").toObject().value("structuredContent")
        .toObject().value("error").toObject().value("code").toString(),
        QStringLiteral("COMMAND_CONFIRMATION_STALE"));
    QVERIFY(!QFile::exists(targetPath));

    QTimer::singleShot(500, &transport, [&transport] {
        emit transport.readyRead(QByteArrayLiteral(
            "sh ./mrtr_script.sh\r\nMRTR_OUTPUT_OK\r\n"
            "\x1b]633;NT;PROMPT;2;0\x07"));
    });
    const int retryId = ++host.counter;
    host.send({{"jsonrpc", "2.0"}, {"id", retryId}, {"method", "tools/call"},
        {"params", QJsonObject{{"name", "novaterm_run_script"},
            {"arguments", arguments}, {"requestState", requestState},
            {"inputResponses", QJsonObject{{"confirm", QJsonObject{{"action", "accept"},
                {"content", QJsonObject{{"confirmed", true}}}}}}}}}});
    const auto completed = host.next();
    QCOMPARE(completed.value("id").toInt(), retryId);
    QCOMPARE(completed.value("result").toObject().value("resultType").toString(),
             QStringLiteral("complete"));
    const auto completedPayload = completed.value("result").toObject()
        .value("structuredContent").toObject();
    QVERIFY2(completedPayload.value("ok").toBool(),
        qPrintable(completedPayload.value("error").toObject().value("code").toString()));
    QFile file(targetPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), body.toUtf8());
    QVERIFY(transport.written.contains("sh ./mrtr_script.sh"));
    QVERIFY(!transport.written.contains("MRTR_BODY_PRIVATE"));
    QVERIFY(completedPayload.value("data").toObject().value("stdout").toString()
        .contains(QStringLiteral("MRTR_OUTPUT_OK")));

    const qsizetype writesAfterExecution = transport.written.size();
    const int replayId = ++host.counter;
    host.send({{"jsonrpc", "2.0"}, {"id", replayId}, {"method", "tools/call"},
        {"params", QJsonObject{{"name", "novaterm_run_script"},
            {"arguments", arguments}, {"requestState", requestState},
            {"inputResponses", QJsonObject{{"confirm", QJsonObject{{"action", "accept"},
                {"content", QJsonObject{{"confirmed", true}}}}}}}}}});
    const auto replay = host.next();
    QCOMPARE(replay.value("result").toObject().value("structuredContent")
        .toObject().value("error").toObject().value("code").toString(),
        QStringLiteral("COMMAND_CONFIRMATION_STALE"));
    QCOMPARE(transport.written.size(), writesAfterExecution);
}

void McpTests::userTypingCancelsInFlightScriptUploadBeforeInvocation()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::LocalShell;
    runtime.transport.insert(QStringLiteral("interactiveShellKind"), QStringLiteral("posix"));
    TerminalSession session(runtime);
    LocalFake transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed,
                   TransportKind::LocalShell);
    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    Service service(root.filePath(QStringLiteral("state")),
                    root.filePath(QStringLiteral("instances")),
                    std::make_unique<MemoryCredentialStore>());
    service.directory().add(&session);
    auto delayedProvider = std::make_unique<DelayedScriptProvider>();
    auto* delayed = delayedProvider.get();
    session.installScriptProvider(std::move(delayedProvider));
    const auto client = service.access().addClient(QStringLiteral("fixture"));
    QVERIFY(service.access().setEnabled(true));
    const auto entry = service.directory().entries().first();
    QVERIFY(service.access().setGrant(client, entry, true, {}, true, false, true));
    QTRY_COMPARE(service.status(), QStringLiteral("Listening"));
    emit transport.readyRead(QByteArrayLiteral("\x1b]633;NT;PROMPT;1;0\x07"));
    QTRY_VERIFY(session.commandCoordinator()->isPromptReady());
    Host host;
    QVERIFY(host.start(root.filePath(QStringLiteral("instances")),
        service.access().exportToken(client).value_or(QByteArray{}), {}, true));

    const QString target = root.filePath(QStringLiteral("never-executed.sh"));
    const QJsonObject arguments{{"sessionId", entry.id}, {"epoch", entry.epoch},
        {"scriptContent", QStringLiteral("#!/bin/sh\nprintf private-body\n")},
        {"targetPath", target}, {"workingDirectory", root.path()},
        {"invocation", QStringLiteral("sh ./never-executed.sh")}};
    const int callId = host.begin("novaterm_run_script", arguments);
    const auto confirmation = host.next();
    QCOMPARE(confirmation.value("method").toString(), QStringLiteral("elicitation/create"));
    host.send({{"jsonrpc", "2.0"}, {"id", confirmation.value("id")},
        {"result", QJsonObject{{"action", "accept"},
            {"content", QJsonObject{{"confirmed", true}}}}}});
    QTRY_COMPARE(delayed->writeCount, 1);

    session.inputArbiter()->submitUserInput(QByteArrayLiteral("user owns terminal"));

    const auto cancelled = host.next();
    QCOMPARE(cancelled.value("id").toInt(), callId);
    QCOMPARE(cancelled.value("result").toObject().value("structuredContent")
        .toObject().value("error").toObject().value("code").toString(),
        QStringLiteral("SCRIPT_CANCELLED_BY_USER"));
    QCOMPARE(delayed->cancelCount, 1);
    QCOMPARE(transport.written, QByteArrayLiteral("user owns terminal"));
    QVERIFY(!transport.written.contains("sh ./never-executed.sh"));
    QVERIFY(!transport.written.contains("private-body"));
    QVERIFY(!QFile::exists(target));
}

void McpTests::commandAndScriptProductGrantsAreIndependent()
{
    Fixture fixture;
    QVERIFY(fixture.enable());
    const auto entry = fixture.service.directory().entries().first();

    QVERIFY(fixture.service.access().setGrant(fixture.client, entry,
        true, {}, false, false, false));
    QVERIFY(fixture.service.access().canRead(fixture.client, entry));
    QVERIFY(!fixture.service.access().canRunCommand(fixture.client, entry));
    QVERIFY(!fixture.service.access().canRunConfirmedCommand(fixture.client, entry));
    QVERIFY(!fixture.service.access().canRunScriptTask(fixture.client, entry));

    QVERIFY(fixture.service.access().setGrant(fixture.client, entry,
        true, {}, true, false, false));
    QVERIFY(fixture.service.access().canRunCommand(fixture.client, entry));
    QVERIFY(!fixture.service.access().canRunConfirmedCommand(fixture.client, entry));
    QVERIFY(!fixture.service.access().canRunScriptTask(fixture.client, entry));

    QVERIFY(fixture.service.access().setGrant(fixture.client, entry,
        true, {}, true, true, false));
    QVERIFY(fixture.service.access().canRunConfirmedCommand(fixture.client, entry));
    QVERIFY(!fixture.service.access().canRunScriptTask(fixture.client, entry));

    QVERIFY(fixture.service.access().setGrant(fixture.client, entry,
        true, {}, true, false, true));
    QVERIFY(!fixture.service.access().canRunConfirmedCommand(fixture.client, entry));
    QVERIFY(fixture.service.access().canRunScriptTask(fixture.client, entry));
}

void McpTests::lowRiskCommandUsesCurrentInteractiveTransport()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Serial;
    runtime.transport.insert(QStringLiteral("interactivePromptPattern"),
                             QStringLiteral("device> $"));
    TerminalSession session(runtime);
    LocalFake transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed,
                   TransportKind::Serial);
    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    Service service(root.filePath(QStringLiteral("state")),
                    root.filePath(QStringLiteral("instances")),
                    std::make_unique<MemoryCredentialStore>());
    service.directory().add(&session);
    const QString client = service.access().addClient(QStringLiteral("interactive"));
    QVERIFY(!client.isEmpty());
    QVERIFY(service.access().setEnabled(true));
    const auto entry = service.directory().entries().first();
    QVERIFY(service.access().setGrant(client, entry, true));
    QTRY_COMPARE(service.status(), QStringLiteral("Listening"));

    emit transport.readyRead(QByteArrayLiteral("device> "));
    QTRY_VERIFY_WITH_TIMEOUT(session.commandCoordinator()->isPromptReady(), 500);
    Host host;
    QVERIFY(host.start(root.filePath(QStringLiteral("instances")),
                       service.access().exportToken(client).value_or(QByteArray{})));
    const auto listed = host.call("novaterm_list_sessions", {})
                            .value("data").toObject()
                            .value("sessions").toArray().first().toObject();
    QVERIFY(listed.value("capabilities").toArray().contains(
        QStringLiteral("run_command")));
    auto arguments = QJsonObject{{"sessionId", entry.id}, {"epoch", entry.epoch},
                                 {"command", QStringLiteral("pwd")}};
    QTimer::singleShot(50, &transport, [&transport] {
        emit transport.readyRead(QByteArrayLiteral("pwd\r\n/root\r\ndevice> "));
    });
    const auto response = host.call("novaterm_run_command", arguments);
    QVERIFY2(response.value("ok").toBool(),
             qPrintable(response.value("error").toObject().value("code").toString()));
    QCOMPARE(transport.written, QByteArrayLiteral("pwd\r"));
    QVERIFY(response.value("data").toObject().value("stdout")
        .toString().contains(QStringLiteral("/root")));
}

void McpTests::unverifiedSshCommandUsesCurrentTerminal()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Ssh;
    TerminalSession session(runtime);
    LocalFake transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed,
                   TransportKind::Ssh);
    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    Service service(root.filePath(QStringLiteral("state")),
                    root.filePath(QStringLiteral("instances")),
                    std::make_unique<MemoryCredentialStore>());
    service.directory().add(&session);
    auto isolated = std::make_unique<TestCommandExecutor>();
    auto* isolatedPtr = isolated.get();
    session.commandFacade()->installExecutor(std::move(isolated),
                                             session.statistics().generation);
    const auto entry = service.directory().entries().first();
    const QString client = service.access().addClient(QStringLiteral("unverified"));
    QVERIFY(!client.isEmpty());
    QVERIFY(service.access().setEnabled(true));
    QVERIFY(service.access().setGrant(client, entry, true));
    QTRY_COMPARE(service.status(), QStringLiteral("Listening"));
    Host host;
    QVERIFY(host.start(root.filePath(QStringLiteral("instances")),
        service.access().exportToken(client).value_or(QByteArray{})));
    const auto listed = host.call("novaterm_list_sessions", {})
        .value("data").toObject().value("sessions").toArray().first().toObject();
    QVERIFY(listed.value("capabilities").toArray().contains(
        QStringLiteral("run_command")));

    const int callId = host.begin("novaterm_run_command",
        QJsonObject{{"sessionId", entry.id}, {"epoch", entry.epoch},
                    {"command", QStringLiteral("pwd")}});
    QTRY_VERIFY_WITH_TIMEOUT(transport.written.startsWith(QByteArrayLiteral("pwd;")), 1000);
    const qsizetype start = transport.written.indexOf(QByteArrayLiteral("NT;END;"));
    const qsizetype end = transport.written.indexOf(QByteArrayLiteral(";%d"), start);
    QVERIFY(start >= 0 && end > start + 7);
    const QByteArray nonce = transport.written.mid(start + 7, end - start - 7);
    emit transport.readyRead(QByteArrayLiteral("root# ") + transport.written
        + QByteArrayLiteral("\n/root\n\x1b]633;NT;END;") + nonce
        + QByteArrayLiteral(";0\x07"));
    const auto response = host.next();
    QCOMPARE(response.value("id").toInt(), callId);
    QVERIFY2(response.value("result").toObject().value("structuredContent")
        .toObject().value("ok").toBool(), qPrintable(QJsonDocument(response).toJson()));
    QCOMPARE(isolatedPtr->submissionCount, 0);
    QVERIFY(session.core()->waitForIdle());
    const auto snapshot = session.core()->terminalState();
    QByteArray visible;
    for (const auto& line : snapshot.viewport)
        visible += QByteArray::fromStdString(line.text);
    QVERIFY(visible.contains(QByteArrayLiteral("root# pwd")));
    QVERIFY(visible.contains(QByteArrayLiteral("/root")));
    QVERIFY(!visible.contains(QByteArrayLiteral("__nvterm")));
}

void McpTests::unverifiedSshRiskyCommandRequiresHumanConfirmation()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Ssh;
    TerminalSession session(runtime);
    LocalFake transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed,
                   TransportKind::Ssh);
    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    Service service(root.filePath(QStringLiteral("state")),
                    root.filePath(QStringLiteral("instances")),
                    std::make_unique<MemoryCredentialStore>());
    service.directory().add(&session);
    session.commandFacade()->installExecutor(
        std::make_unique<TestCommandExecutor>(),
        session.statistics().generation);
    const auto entry = service.directory().entries().first();
    const QString client = service.access().addClient(QStringLiteral("confirmed"));
    QVERIFY(!client.isEmpty());
    QVERIFY(service.access().setEnabled(true));
    QVERIFY(service.access().setGrant(client, entry, true));
    QTRY_COMPARE(service.status(), QStringLiteral("Listening"));
    Host host;
    QVERIFY(host.start(root.filePath(QStringLiteral("instances")),
        service.access().exportToken(client).value_or(QByteArray{}), {}, true));

    const int callId = host.begin("novaterm_run_command",
        QJsonObject{{"sessionId", entry.id}, {"epoch", entry.epoch},
                    {"command", QStringLiteral("rm -rf ./cache")}});
    const auto prompt = host.next();
    QCOMPARE(prompt.value("method").toString(), QStringLiteral("elicitation/create"));
    QVERIFY(transport.written.isEmpty());
    host.send({{"jsonrpc", "2.0"}, {"id", prompt.value("id")},
        {"result", QJsonObject{{"action", "accept"},
            {"content", QJsonObject{{"confirmed", true}}}}}});
    QTRY_VERIFY_WITH_TIMEOUT(transport.written.startsWith(
        QByteArrayLiteral("rm -rf ./cache;")), 1000);
    const qsizetype start = transport.written.indexOf(QByteArrayLiteral("NT;END;"));
    const qsizetype end = transport.written.indexOf(QByteArrayLiteral(";%d"), start);
    QVERIFY(start >= 0 && end > start + 7);
    const QByteArray nonce = transport.written.mid(start + 7, end - start - 7);
    emit transport.readyRead(QByteArrayLiteral("root# ") + transport.written
        + QByteArrayLiteral("\n\x1b]633;NT;END;") + nonce
        + QByteArrayLiteral(";0\x07"));
    const auto response = host.next();
    QCOMPARE(response.value("id").toInt(), callId);
    QVERIFY(response.value("result").toObject().value("structuredContent")
        .toObject().value("ok").toBool());

    const int staleId = host.begin("novaterm_run_command",
        QJsonObject{{"sessionId", entry.id}, {"epoch", entry.epoch},
                    {"command", QStringLiteral("rm -rf ./cache")}});
    const auto stalePrompt = host.next();
    QCOMPARE(stalePrompt.value("method").toString(), QStringLiteral("elicitation/create"));
    const QByteArray submittedBeforeTyping = transport.written;
    session.inputArbiter()->submitUserInput(QByteArrayLiteral("x"));
    host.send({{"jsonrpc", "2.0"}, {"id", stalePrompt.value("id")},
        {"result", QJsonObject{{"action", "accept"},
            {"content", QJsonObject{{"confirmed", true}}}}}});
    const auto stale = host.next();
    QCOMPARE(stale.value("id").toInt(), staleId);
    QCOMPARE(stale.value("result").toObject().value("structuredContent")
        .toObject().value("error").toObject().value("code").toString(),
        QStringLiteral("COMMAND_CONFIRMATION_STALE"));
    QCOMPARE(transport.written, submittedBeforeTyping + QByteArrayLiteral("x"));
}

void McpTests::riskyCommandRequiresHumanConfirmation()
{
    Fixture fixture;
    QVERIFY(fixture.enable());
    QTRY_COMPARE(fixture.service.status(), QStringLiteral("Listening"));
    Host host;
    QVERIFY(host.start(fixture.runtime(), fixture.token()));

    auto arguments = fixture.identity();
    arguments.insert(QStringLiteral("command"),
                     QStringLiteral("rm -rf ./cache"));
    const auto needsHuman = host.call("novaterm_run_command", arguments);
    QCOMPARE(needsHuman.value("error").toObject().value("code").toString(),
             QStringLiteral("CLIENT_CONFIRMATION_UNAVAILABLE"));
    arguments.insert(QStringLiteral("command"),
                     QStringLiteral("sudo systemctl stop auditd"));
    const auto denied = host.call("novaterm_run_command", arguments);
    QCOMPARE(denied.value("error").toObject().value("code").toString(),
             QStringLiteral("COMMAND_NOT_ALLOWED"));
    QVERIFY(fixture.local.written.isEmpty());
}

void McpTests::interactiveGrantIsSeparateFromReadAndFixedCommands()
{
    Fixture fixture;
    QVERIFY(fixture.enable());
    const auto entry = fixture.service.directory().entries().first();
    QVERIFY(fixture.service.access().canRead(fixture.client, entry));
    QVERIFY(!fixture.service.access().canRunCommand(fixture.client, entry));

    QVERIFY(fixture.service.access().setGrant(fixture.client, entry,
                                             true, {}, true));
    QVERIFY(fixture.service.access().canRunCommand(fixture.client, entry));
    QVERIFY(fixture.service.access().commands(fixture.client, entry).isEmpty());
    QVERIFY(fixture.service.access().setGrant(fixture.client, entry,
                                             true, {}, false));
    QVERIFY(!fixture.service.access().canRunCommand(fixture.client, entry));
}

void McpTests::freeCommandSchemaIsBoundedAndStrict()
{
    const QJsonObject base{
        {QStringLiteral("sessionId"),
         QStringLiteral("7abf0022-9546-4c2b-a046-5b40589d38ec")},
        {QStringLiteral("epoch"), QStringLiteral("epoch-1")},
        {QStringLiteral("command"), QStringLiteral("pwd")}};
    QVERIFY(validateArguments(QStringLiteral("novaterm_run_command"),
                              base).isEmpty());
    auto invalid = base;
    invalid.insert(QStringLiteral("command"), QString{});
    QVERIFY(!validateArguments(QStringLiteral("novaterm_run_command"),
                               invalid).isEmpty());
    invalid = base;
    invalid.insert(QStringLiteral("command"), QString(16385, QLatin1Char('x')));
    QVERIFY(!validateArguments(QStringLiteral("novaterm_run_command"),
                               invalid).isEmpty());
    invalid = base;
    invalid.insert(QStringLiteral("stdin"), QStringLiteral("secret"));
    QVERIFY(!validateArguments(QStringLiteral("novaterm_run_command"),
                               invalid).isEmpty());
}

void McpTests::commandRiskPolicyDefaultsToHumanConfirmation()
{
    CommandRiskPolicy policy;
    QCOMPARE(policy.classify(QStringLiteral("uname -srm")).decision,
             RiskDecision::Allow);
    QCOMPARE(policy.classify(QStringLiteral("uptime")).decision,
             RiskDecision::Allow);
    QCOMPARE(policy.classify(QStringLiteral("free -k")).decision,
             RiskDecision::Allow);
    QCOMPARE(policy.classify(QStringLiteral("df -Pk")).decision,
             RiskDecision::Allow);
    QCOMPARE(policy.classify(QStringLiteral("whoami")).decision,
             RiskDecision::Allow);
    QCOMPARE(policy.classify(QStringLiteral("ls")).decision,
             RiskDecision::Allow);
    QCOMPARE(policy.classify(QStringLiteral("ls -la")).decision,
             RiskDecision::Allow);
    QCOMPARE(policy.classify(QStringLiteral("cd /tmp")).decision,
             RiskDecision::Allow);
    QCOMPARE(policy.classify(QStringLiteral("cat /etc/os-release")).decision,
             RiskDecision::Allow);
    QCOMPARE(policy.classify(QStringLiteral("cat /proc/version")).decision,
             RiskDecision::Allow);
    QCOMPARE(policy.classify(QStringLiteral("cat /etc/shadow")).decision,
             RiskDecision::Deny);
    QCOMPARE(policy.classify(QStringLiteral("rm -rf ./cache")).decision,
             RiskDecision::Confirm);
    QCOMPARE(policy.classify(QStringLiteral("rm -rf /etc")).decision,
             RiskDecision::Confirm);
    QCOMPARE(policy.classify(QStringLiteral("sudo systemctl stop auditd")).decision,
             RiskDecision::Deny);
    QCOMPARE(policy.classify(QStringLiteral("python -c 'dynamic()'")).decision,
             RiskDecision::Confirm);
    QCOMPARE(policy.classify(QStringLiteral("unknown-tool --do-work")).decision,
             RiskDecision::Unknown);
    QCOMPARE(policy.classify(QStringLiteral("uname -srm; rm x")).decision,
             RiskDecision::Confirm);
}

void McpTests::publishedSnapshotSwitchDefaultsToEnabled()
{
    const QByteArray previous = qgetenv("NOVATERM_MCP_PUBLISHED_SNAPSHOT");
    const auto restore = qScopeGuard([previous] {
        if (previous.isNull())
            qunsetenv("NOVATERM_MCP_PUBLISHED_SNAPSHOT");
        else
            qputenv("NOVATERM_MCP_PUBLISHED_SNAPSHOT", previous);
    });
    // 未设置必须等于「启用」。qEnvironmentVariableIntValue 在变量缺失时返回 0，
    // 判据若只比较数值就会把默认启用悄悄退化成默认关闭。
    qunsetenv("NOVATERM_MCP_PUBLISHED_SNAPSHOT");
    QVERIFY(NovaTerm::publishedContextSnapshotEnabled());
    qputenv("NOVATERM_MCP_PUBLISHED_SNAPSHOT", "0");
    QVERIFY(!NovaTerm::publishedContextSnapshotEnabled());
    qputenv("NOVATERM_MCP_PUBLISHED_SNAPSHOT", "1");
    QVERIFY(NovaTerm::publishedContextSnapshotEnabled());
    qputenv("NOVATERM_MCP_PUBLISHED_SNAPSHOT", "2");
    QVERIFY(NovaTerm::publishedContextSnapshotEnabled());
    // 关闭开关时不得向解析器请求发布。
    qputenv("NOVATERM_MCP_PUBLISHED_SNAPSHOT", "0");
    TerminalCore core{80, 24};
    TerminalContextProvider provider{&core};
    QVERIFY(core.waitForIdle());
    QVERIFY(provider.trySnapshot());
    QCOMPARE(core.publishedContextStatistics().requestCount, 0u);
}

void McpTests::publishedSnapshotPreservesCaptureTime()
{
    const QByteArray previous = qgetenv("NOVATERM_MCP_PUBLISHED_SNAPSHOT");
    const auto restore = qScopeGuard([previous] {
        if (previous.isNull())
            qunsetenv("NOVATERM_MCP_PUBLISHED_SNAPSHOT");
        else
            qputenv("NOVATERM_MCP_PUBLISHED_SNAPSHOT", previous);
    });
    qputenv("NOVATERM_MCP_PUBLISHED_SNAPSHOT", "1");

    TerminalCore core{80, 24};
    TerminalContextProvider provider{&core};
    QVERIFY(core.waitForIdle());
    // try-read 是主路径：模型空闲时直接取到最新数据，根本不必请求发布。
    const auto first = provider.trySnapshot();
    QVERIFY(first);
    QCOMPARE(core.publishedContextStatistics().requestCount, 0u);
    // 同一 revision 的重复读取复用同一份不可变快照，capturedAt 不得被刷新
    // 成「刚刚捕获」——它是底层快照真实形成的时间。
    QTest::qWait(20);
    const auto second = provider.trySnapshot();
    QVERIFY(second);
    QCOMPARE(second.get(), first.get());
    QCOMPARE(second->capturedAt, first->capturedAt);
    QCOMPARE(second->state.revision, first->state.revision);
    // 新内容出现后必须立即反映新 revision，而不是停在旧快照上。
    core.writeInput(QByteArrayLiteral("more\r\n"));
    QVERIFY(core.waitForIdle());
    const auto refreshed = provider.trySnapshot();
    QVERIFY(refreshed);
    QVERIFY(refreshed->state.revision > first->state.revision);

    // 发布物兜底只在模型被持续输出占满时触发（try-read 失败），单元测试的空闲
    // 核心无法稳定构造该状态；该路径由 tests/mcp/performance_check.py 在持续
    // 输出的同轮 A/B 中覆盖（coreContextPublishCount > 0 才算覆盖到）。
    qputenv("NOVATERM_MCP_PUBLISHED_SNAPSHOT", "0");
    TerminalContextProvider legacy{&core};
    const auto before = core.publishedContextStatistics().requestCount;
    QVERIFY(legacy.trySnapshot());
    QCOMPARE(core.publishedContextStatistics().requestCount, before);
}

void McpTests::sessionInvalidationCompletesPendingExecution()
{
    Fixture fixture;
    auto state = std::make_shared<PendingExecutorState>();
    fixture.session.commandFacade()->installExecutor(
        std::make_unique<PendingCommandExecutor>(state),
        fixture.session.statistics().generation);
    static_cast<void>(fixture.service.directory().entries());
    QVERIFY(fixture.enable(true));
    QTRY_COMPARE(fixture.service.status(), QStringLiteral("Listening"));

    Host host;
    QVERIFY(host.start(fixture.runtime(), fixture.token()));
    const auto catalog = host.call("novaterm_list_commands", fixture.identity())
                             .value("data").toObject();
    const auto command = catalog.value("commands").toArray().first().toObject();
    auto arguments = fixture.identity();
    arguments.insert("commandId", command.value("commandId"));
    arguments.insert("commandTicket", command.value("commandTicket"));
    arguments.insert("policyVersion", catalog.value("policyVersion"));
    arguments.insert("arguments", QJsonObject{});
    host.begin("novaterm_execute_command", arguments);
    QTRY_COMPARE(state->submissions, 1);

    fixture.session.close();
    static_cast<void>(host.next());

    QTRY_VERIFY(!fixture.service.executionRecords().isEmpty());
    QTRY_VERIFY(fixture.service.executionRecords().first().toObject()
                    .value("complete").toBool());
    QVERIFY(!state->cancelledRequests.isEmpty());
    for (const quint64 requestId : std::as_const(state->cancelledRequests))
        QCOMPARE(requestId, state->cancelledRequests.first());
    const auto targets = fixture.service.protectedTargets();
    QCOMPARE(targets.size(), 1);
    const QString fingerprint = targets.begin().key();
    QVERIFY(fixture.service.acknowledgeTarget(fingerprint));
    QVERIFY(fixture.service.protectedTargets().isEmpty());
}

void McpTests::localShellCommandsRunOutsideInteractiveTransport()
{
    Fixture fixture;
    QVERIFY(fixture.enable(true));
    QTRY_COMPARE(fixture.service.status(), QStringLiteral("Listening"));
    Host host;
    QVERIFY(host.start(fixture.runtime(), fixture.token()));

    const auto catalog = host.call("novaterm_list_commands", fixture.identity())
                             .value("data").toObject();
    QVERIFY2(catalog.value("executionEnabled").toBool(),
             qPrintable(catalog.value("disabledReason").toString()));
    QCOMPARE(catalog.value("commands").toArray().size(), 4);
    QJsonObject command;
    for (const auto& value : catalog.value("commands").toArray()) {
        const auto candidate = value.toObject();
        if (candidate.value("commandId") == QStringLiteral("system.identity")) {
            command = candidate;
            break;
        }
    }
    QVERIFY(!command.isEmpty());

    auto arguments = fixture.identity();
    arguments.insert("commandId", command.value("commandId"));
    arguments.insert("commandTicket", command.value("commandTicket"));
    arguments.insert("policyVersion", catalog.value("policyVersion"));
    arguments.insert("arguments", QJsonObject{});
    const auto completed = host.call("novaterm_execute_command", arguments);
    QVERIFY(completed.value("ok").toBool());
    QVERIFY(!completed.value("data").toObject().value("stdout")
                 .toString().isEmpty());
    QCOMPARE(host.call("novaterm_execute_command", arguments), completed);
    QVERIFY(fixture.local.written.isEmpty());
}

void McpTests::localFixedDiagnosticsStayIsolatedWithTrustedShellProfile()
{
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    eApp->init();
    qRegisterMetaType<CommandExecutionResult>();
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::LocalShell;
    runtime.transport.insert(QStringLiteral("interactiveShellKind"),
                             QStringLiteral("posix"));
    TerminalSession session(runtime);
    LocalFake transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed,
                   TransportKind::LocalShell);
    QVERIFY(session.start());
    SessionDirectory directory;
    directory.add(&session);
    auto* facade = session.commandFacade();
    QVERIFY(facade && facade->isAvailable());
    QCOMPARE(facade->capabilities().mode, CommandExecutionMode::Isolated);
    QCOMPARE(facade->profile().version(),
             CommandPlatformProfile::forTransport(TransportKind::LocalShell).version());
    QSignalSpy finished{facade, &SessionCommandFacade::finished};
    CommandExecutionRequest request;
    request.requestId = 1;
    request.commandId = QString(NovaTerm::LocalDiagnostic::SystemIdentity);
    QVERIFY(facade->execute(request));
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY(transport.written.isEmpty());

    QTemporaryDir root;
    Service service(root.filePath(QStringLiteral("state")),
                    root.filePath(QStringLiteral("instances")),
                    std::make_unique<MemoryCredentialStore>());
    service.directory().add(&session);
    const QString client = service.access().addClient(QStringLiteral("fixture"));
    QVERIFY(service.access().setEnabled(true));
    QTRY_COMPARE(service.status(), QStringLiteral("Listening"));
    QWidget owner;
    QPointer<McpSettingsDialog> dialog = new McpSettingsDialog(&service, &owner);
    dialog->show();
    auto* tree = dialog->findChild<ElaTreeWidget*>();
    QVERIFY(tree && tree->topLevelItemCount() == 1);
    for (const int column : {2, 3}) {
        const QRect cell = tree->visualRect(
            tree->indexFromItem(tree->topLevelItem(0), column));
        QVERIFY(cell.width() > 30);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(cell.right() - 4, cell.center().y()));
    }
    QTRY_COMPARE(service.access().commands(
        client, service.directory().entries().first()).size(), 4);
    QCOMPARE(tree->topLevelItem(0)->checkState(3), Qt::Checked);
    dialog->close();
    QTRY_VERIFY(dialog.isNull());
#else
    QSKIP("Local fixed diagnostics are not available on this platform.");
#endif
}

void McpTests::localSessionExecutorKeepsCommandsIsolatedAndBounded()
{
    qRegisterMetaType<CommandExecutionResult>();
    LocalFake interactiveTransport;
    LocalSessionCommandExecutor executor{
        QString::fromUtf8(NOVATERM_LOCAL_EXECUTOR_TEST_CHILD)};
    QSignalSpy finished{&executor, &ISessionCommandExecutor::finished};

    const auto run = [&](quint64 requestId, QStringView commandId,
                         CommandExecutionLimits limits) -> CommandExecutionResult {
        finished.clear();
        CommandExecutionRequest request;
        request.requestId = requestId;
        request.limits = limits;
        request.commandId = commandId.toString();
        if (!executor.execute(request))
            return {};
        QElapsedTimer timer;
        timer.start();
        while (finished.count() == 0 && timer.elapsed() < 3000)
            QTest::qWait(5);
        return finished.count() == 1
            ? qvariant_cast<CommandExecutionResult>(finished.takeFirst().at(0))
            : CommandExecutionResult{};
    };

    const auto success = run(101, NovaTerm::LocalDiagnostic::SystemIdentity,
                             {64, 5000});
    QCOMPARE(success.requestId, quint64{101});
    QCOMPARE(success.outcome, CommandExecutionOutcome::Completed);
    QVERIFY(success.executionMayHaveStarted);
    QVERIFY(success.terminationConfirmed);
    QCOMPARE(success.exitCode, std::optional<int>{0});
    QCOMPARE(success.standardOutput.trimmed(), QByteArray("fixture-ok"));

    const auto failed = run(102, NovaTerm::LocalDiagnostic::SystemUptime,
                            {64, 5000});
    QCOMPARE(failed.requestId, quint64{102});
    QCOMPARE(failed.outcome, CommandExecutionOutcome::Failed);
    QVERIFY(failed.terminationConfirmed);
    QCOMPARE(failed.exitCode, std::optional<int>{42});
    QCOMPARE(failed.standardError.trimmed(), QByteArray("fixture-failed"));

    const auto limited = run(103, NovaTerm::LocalDiagnostic::MemorySummary,
                             {64, 5000});
    QCOMPARE(limited.requestId, quint64{103});
    QCOMPARE(limited.outcome, CommandExecutionOutcome::OutputLimit);
    QVERIFY(limited.outputTruncated);
    QVERIFY(limited.terminationConfirmed);
    QVERIFY(limited.standardOutput.size() + limited.standardError.size() <= 64);

    const auto timedOut = run(104, NovaTerm::LocalDiagnostic::FilesystemUsage,
                              {64, 50});
    QCOMPARE(timedOut.requestId, quint64{104});
    QCOMPARE(timedOut.outcome, CommandExecutionOutcome::TimedOut);
    QVERIFY(timedOut.executionMayHaveStarted);
    QVERIFY(timedOut.terminationConfirmed);

    finished.clear();
    CommandExecutionRequest cancelledRequest;
    cancelledRequest.requestId = 105;
    cancelledRequest.limits = {64, 5000};
    cancelledRequest.commandId = QString(NovaTerm::LocalDiagnostic::FilesystemUsage);
    QVERIFY(executor.execute(cancelledRequest));
    QVERIFY(!executor.execute(CommandExecutionRequest{
        106, {}, {64, 1000},
        QString(NovaTerm::LocalDiagnostic::SystemIdentity)}));
    executor.cancel(105);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 3000);
    const auto cancelled = qvariant_cast<CommandExecutionResult>(
        finished.takeFirst().at(0));
    QCOMPARE(cancelled.outcome, CommandExecutionOutcome::Cancelled);
    QVERIFY(cancelled.terminationConfirmed);
    QVERIFY(interactiveTransport.written.isEmpty());
}

void McpTests::localDiagnosticHelperAcceptsOnlyFixedCommands()
{
    const auto run = [](const QStringList& arguments) {
        QProcess process;
        process.start(QString::fromUtf8(NOVATERM_LOCAL_DIAG), arguments);
        const bool started = process.waitForStarted(3000);
        const bool finished = started && process.waitForFinished(5000);
        return std::tuple{started, finished, process.exitStatus(),
                          process.exitCode(), process.readAllStandardOutput(),
                          process.readAllStandardError()};
    };

    const QStringList commands{
        QString(NovaTerm::LocalDiagnostic::SystemIdentity),
        QString(NovaTerm::LocalDiagnostic::SystemUptime),
        QString(NovaTerm::LocalDiagnostic::MemorySummary),
        QString(NovaTerm::LocalDiagnostic::FilesystemUsage),
    };
    for (const QString& command : commands) {
        QVERIFY(NovaTerm::LocalDiagnostic::isKnownCommand(command));
        const auto [started, finished, status, exitCode, output, error] =
            run({command});
        QVERIFY(started);
        QVERIFY(finished);
        QCOMPARE(status, QProcess::NormalExit);
        QCOMPARE(exitCode, 0);
        QVERIFY(!output.isEmpty());
        QVERIFY(error.isEmpty());
        QVERIFY(output.size() <= NovaTerm::LocalDiagnostic::MaxOutputBytes);
    }

    for (const QStringList& invalid : {
             QStringList{QStringLiteral("unknown.command")},
             QStringList{QString(NovaTerm::LocalDiagnostic::SystemIdentity),
                         QStringLiteral("extra")},
             QStringList{QStringLiteral("system.identity && whoami")},
         }) {
        const auto [started, finished, status, exitCode, output, error] =
            run(invalid);
        QVERIFY(started);
        QVERIFY(finished);
        QCOMPARE(status, QProcess::NormalExit);
        QCOMPARE(exitCode, 2);
        QVERIFY(output.isEmpty());
        QVERIFY(!error.isEmpty());
    }
}

void McpTests::sessionCommandFacadeCancelsRequestsBeforeRebinding()
{
    SessionCommandFacade facade;
    auto cancelledRequests = std::make_shared<QList<quint64>>();
    facade.installExecutor(
        std::make_unique<TestCommandExecutor>(cancelledRequests), 1);
    QVERIFY(facade.execute(CommandExecutionRequest{41, "fixed", {64, 1000}}));

    facade.reset(2);

    QCOMPARE(*cancelledRequests, QList<quint64>{41});
}

void McpTests::registeredSessionExecutorIsUsedWithoutTransportCast()
{
    Fixture fixture;
    auto executor = std::make_unique<TestCommandExecutor>();
    auto* observedExecutor = executor.get();
    fixture.session.commandFacade()->installExecutor(
        std::move(executor), fixture.session.statistics().generation);
    static_cast<void>(fixture.service.directory().entries());
    QVERIFY(fixture.enable(true));
    QTRY_COMPARE(fixture.service.status(), QStringLiteral("Listening"));

    Host host;
    QVERIFY(host.start(fixture.runtime(), fixture.token()));
    const auto catalog = host.call("novaterm_list_commands", fixture.identity())
                             .value("data").toObject();
    QVERIFY2(catalog.value("executionEnabled").toBool(),
             qPrintable(catalog.value("disabledReason").toString()));
    QCOMPARE(catalog.value("commands").toArray().size(), 4);

    const auto command = catalog.value("commands").toArray().first().toObject();
    auto arguments = fixture.identity();
    arguments.insert("commandId", command.value("commandId"));
    arguments.insert("commandTicket", command.value("commandTicket"));
    arguments.insert("policyVersion", catalog.value("policyVersion"));
    arguments.insert("arguments", QJsonObject{});
    const auto response = host.call("novaterm_execute_command", arguments);
    QVERIFY(response.value("ok").toBool());
    QCOMPARE(observedExecutor->submissionCount, 1);
    QCOMPARE(observedExecutor->lastRequest.command,
             QByteArrayLiteral("uname -srm"));
    QVERIFY(fixture.local.written.isEmpty());
}

void McpTests::sessionCommandFacadeRoutesTrustedExecutors()
{
    SessionDirectory directory;
    TerminalCore localCore{80, 24};
    TerminalSession localSession{&localCore};
    LocalFake local;
    localSession.attach(&local, TerminalSession::Ownership::Borrowed,
                        TransportKind::LocalShell);
    directory.add(&localSession);
    auto* localFacade = localSession.commandFacade();
    QVERIFY(localFacade);
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    QVERIFY(localFacade->isAvailable());
#ifdef Q_OS_WIN
    QCOMPARE(localFacade->profile().version(), QStringLiteral("windows-local-v1"));
#else
    QCOMPARE(localFacade->profile().version(), QStringLiteral("linux-local-v1"));
#endif
#else
    QVERIFY(!localFacade->isAvailable());
#endif
    QVERIFY(!localFacade->execute(CommandExecutionRequest{1, "fixed", {64, 1000}}));
    QVERIFY(local.written.isEmpty());

    TerminalCore sshCore{80, 24};
    TerminalSession sshSession{&sshCore};
    SshConfig config;
    config.host = QStringLiteral("fixture.invalid");
    config.username = QStringLiteral("fixture");
    SshTransport ssh{config};
    SshTransportTestAccess::connected(ssh);
    sshSession.attach(&ssh, TerminalSession::Ownership::Borrowed,
                      TransportKind::Ssh);
    directory.add(&sshSession);
    auto* sshFacade = sshSession.commandFacade();
    QVERIFY(sshFacade);
    QVERIFY(sshFacade->isAvailable());
    const auto capabilities = sshFacade->capabilities();
    QCOMPARE(capabilities.mode, CommandExecutionMode::Isolated);
    QVERIFY(capabilities.reliableExitCode);
    QVERIFY(capabilities.reliableTermination);
    QVERIFY(capabilities.isolatedOutput);

    QSignalSpy finished{sshFacade, &SessionCommandFacade::finished};
    QVERIFY(sshFacade->execute(CommandExecutionRequest{2, "fixture-ok", {64, 1000}}));
    QCOMPARE(SshTransportTestAccess::queued(ssh), 1);
    QCOMPARE(SshTransportTestAccess::queuedCommand(ssh), QByteArray("fixture-ok"));
    SshTransportTestAccess::finish(ssh, true);
    QCOMPARE(finished.count(), 1);
}

void McpTests::protocolLifecycleAndOversizedInput()
{
    Host host;
    host.process.start(QString::fromUtf8(NOVATERM_MCP_BRIDGE));
    QVERIFY(host.process.waitForStarted(3000));
    host.send({{"jsonrpc", "2.0"}, {"id", "before"}, {"method", "tools/list"}});
    QCOMPARE(host.next().value("error").toObject().value("code").toInt(), -32002);
    host.send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
        {"params", QJsonObject{{"protocolVersion", "unsupported-version"},
            {"capabilities", QJsonObject{}},
            {"clientInfo", QJsonObject{{"name", "fixture"}, {"version", "1"}}}}}});
    QCOMPARE(host.next().value("result").toObject().value("protocolVersion").toString(),
        QString::fromLatin1(ProtocolVersion));
    host.send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    host.send({{"jsonrpc", "2.0"}, {"id", "7"}, {"method", "ping"}});
    host.send({{"jsonrpc", "2.0"}, {"id", 7}, {"method", "ping"}});
    QCOMPARE(host.next().value("id"), QJsonValue("7"));
    QCOMPARE(host.next().value("id"), QJsonValue(7));
    host.send({{"jsonrpc", "2.0"}, {"id", 8}, {"method", "unknown/method"}});
    QCOMPARE(host.next().value("error").toObject().value("code").toInt(), -32601);
    // 无换行的超长输入也必须回收，不能一直等帧尾并增长内存。
    host.process.write(QByteArray(MaxFrameBytes + 1, 'x'));
    QVERIFY(host.process.waitForFinished(5000));
    QCOMPARE(host.process.exitStatus(), QProcess::NormalExit);
}

void McpTests::instanceSelectionAndDisconnectStayExplicit()
{
    Fixture f;
    QVERIFY(f.enable());
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    const auto other = f.runtime() + "/other.json";
    QVERIFY(writePrivateJson(other, {{"instanceId", newId()}, {"ipcVersion", 1},
        {"endpoint", "unused-fixture-endpoint"}}));
    Host ambiguous;
    QVERIFY(ambiguous.start(f.runtime(), f.token()));
    QCOMPARE(ambiguous.call("novaterm_list_sessions").value("error").toObject()
        .value("code").toString(), QStringLiteral("INSTANCE_SELECTION_REQUIRED"));
    const auto configuration = f.service.clientConfiguration(f.client)
        .value("mcpServers").toObject().value("novaterm").toObject();
    QCOMPARE(configuration.value("args").toArray(), QJsonArray({"--instance", f.service.instanceId()}));
    Host selected;
    QVERIFY(selected.start(f.runtime(), f.token(), f.service.instanceId()));
    QVERIFY(selected.call("novaterm_list_sessions").value("ok").toBool());
    QVERIFY(QFile::remove(other));
    QVERIFY(f.service.access().setEnabled(false));
    QCOMPARE(selected.call("novaterm_list_sessions").value("error").toObject()
        .value("code").toString(), QStringLiteral("APP_UNAVAILABLE"));
    QVERIFY(f.enable());
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    // 同一桥接一旦失去已认证实例，即使端点恢复也不能自动改投新连接。
    QCOMPARE(selected.call("novaterm_list_sessions").value("error").toObject()
        .value("code").toString(), QStringLiteral("APP_UNAVAILABLE"));
}

void McpTests::ccSwitchConfigurationIsSingleServerObject()
{
    Fixture f;
    const auto configuration = f.service.ccSwitchConfiguration(f.client);
    QVERIFY(!configuration.value(QStringLiteral("command")).toString().isEmpty());
    QVERIFY(configuration.value(QStringLiteral("args")).isArray());
    QCOMPARE(configuration.value(QStringLiteral("args")).toArray(), QJsonArray{});
    const auto environment = configuration.value(QStringLiteral("env")).toObject();
    QVERIFY(!environment.value(QStringLiteral("NOVATERM_MCP_TOKEN")).toString().isEmpty());
    QCOMPARE(environment.value(QStringLiteral("NOVATERM_MCP_RUNTIME_DIR")).toString(), f.runtime());
    QVERIFY(!configuration.contains(QStringLiteral("novaterm")));
    QVERIFY(!configuration.contains(QStringLiteral("mcpServers")));

    const auto standard = f.service.clientConfiguration(f.client);
    QVERIFY(standard.value(QStringLiteral("mcpServers")).toObject()
        .contains(QStringLiteral("novaterm")));
}

void McpTests::framingAndAuthentication()
{
    FrameReader reader;
    const auto first = frame({{"value", QStringLiteral("中文")}});
    for (qsizetype i = 0; i < first.size(); ++i) QVERIFY(reader.append(first.mid(i, 1)));
    QCOMPARE(reader.take()->value("value").toString(), QStringLiteral("中文"));
    QVERIFY(reader.append(first + first));
    QVERIFY(reader.take());
    QVERIFY(reader.take());
    QVERIFY(!reader.take());
    QByteArray invalid(4, Qt::Uninitialized);
    qToBigEndian<quint32>(quint32(MaxFrameBytes + 1), invalid.data());
    QVERIFY(!reader.append(invalid));
    const auto secret = randomBytes();
    auto token = signToken({{"scope", "test"}}, secret);
    QVERIFY(verifyToken(token, secret));
    QVERIFY(!verifyToken(token, randomBytes()));
    token[4] = token[4] == 'x' ? 'y' : 'x';
    QVERIFY(!verifyToken(token, secret));
}

void McpTests::rejectsDangerousAndOversizedArguments()
{
    QCOMPARE(tools().size(), 7);
    QVERIFY(!CommandPolicy::find("rm"));
    QVERIFY(!CommandPolicy::find("credential.read"));
    const QJsonObject identity{{"sessionId", newId()}, {"epoch", "epoch"}};
    auto arguments = identity;
    arguments.insert("command", "rm -rf /");
    QVERIFY(!validateArguments("novaterm_read_context", arguments).isEmpty());
    arguments = identity;
    arguments.insert("maxBytes", MaxTextBytes + 1);
    QVERIFY(!validateArguments("novaterm_read_context", arguments).isEmpty());
    arguments = identity;
    arguments.insert("maxLines", 0.5);
    QVERIFY(!validateArguments("novaterm_read_context", arguments).isEmpty());
    qsizetype remaining = 5;
    bool truncated = false;
    const auto result = outputText(QStringLiteral("中文测试").toUtf8(), remaining, truncated);
    QCOMPARE(result, QStringLiteral("中"));
    QVERIFY(truncated);
    QVERIFY(result.toUtf8().size() <= 5);
}

void McpTests::nonblockingSnapshotPreservesIndependentProgress()
{
    TerminalCore core(20, 4);
    core.writeInput("first\r\nsecond\r\n");
    QVERIFY(core.waitForIdle());
    TerminalContextProvider provider(&core);
    const auto first = provider.trySnapshot();
    QVERIFY(first);
    const auto second = provider.trySnapshot();
    QCOMPARE(first.get(), second.get());
    QVERIFY(!first->entries.empty());
    auto before = first->state.revision;
    core.writeInput("third\r\n");
    QVERIFY(core.waitForIdle());
    auto after = provider.trySnapshot();
    QVERIFY(after && after->state.revision > before);
    QCOMPARE(first->state.revision, before);
}

void McpTests::stdioWithoutApplicationAndInitialization()
{
    QTemporaryDir directory;
    Host host;
    QVERIFY(host.start(directory.path(), {}));
    host.send({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
    QCOMPARE(host.next().value("result").toObject().value("tools").toArray().size(), 7);
    const auto result = host.call("novaterm_list_sessions");
    QCOMPARE(result.value("error").toObject().value("code").toString(), QStringLiteral("APP_NOT_RUNNING"));
}

void McpTests::readSearchAndClientIsolation()
{
    Fixture f;
    QVERIFY(f.enable());
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    f.core.writeInput("build ok\r\nERROR sample\r\n");
    QVERIFY(f.core.waitForIdle());
    Host a, b;
    QVERIFY(a.start(f.runtime(), f.token()));
    QVERIFY(b.start(f.runtime(), f.token()));
    const auto sessions = a.call("novaterm_list_sessions").value("data").toObject().value("sessions").toArray();
    QCOMPARE(sessions.size(), 1);
    auto arguments = f.identity();
    const auto first = a.call("novaterm_read_context", arguments);
    QVERIFY2(first.value("ok").toBool(), qPrintable(QJsonDocument(first).toJson()));
    const auto second = b.call("novaterm_read_context", arguments);
    const auto one = first.value("data").toObject();
    const auto two = second.value("data").toObject();
    QCOMPARE(one.value("recentOutput"), two.value("recentOutput"));
    QVERIFY(!one.value("recentOutput").toArray().isEmpty());
    arguments.insert("captureId", one.value("captureId"));
    arguments.insert("query", "ERROR");
    const auto found = a.call("novaterm_search_context", arguments);
    QVERIFY(found.value("ok").toBool());
    QVERIFY(!found.value("data").toObject().value("matches").toArray().isEmpty());
    QCOMPARE(b.call("novaterm_search_context", arguments).value("error").toObject().value("code").toString(),
        QStringLiteral("CAPTURE_NOT_AVAILABLE"));
    Host invalid;
    QVERIFY(invalid.start(f.runtime(), QByteArray(64, 'z')));
    QCOMPARE(invalid.call("novaterm_list_sessions").value("error").toObject().value("code").toString(),
        QStringLiteral("UNAUTHORIZED"));
}

void McpTests::truncatedOutputNeverAdvancesCursor()
{
    Fixture f;
    QVERIFY(f.enable());
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    f.core.writeInput("alpha\r\nbeta\r\ngamma\r\n");
    QVERIFY(f.core.waitForIdle());
    Host host;
    QVERIFY(host.start(f.runtime(), f.token()));
    auto arguments = f.identity();
    arguments.insert("maxBytes", 1);
    arguments.insert("maxLines", 1);
    const auto data = host.call("novaterm_read_context", arguments).value("data").toObject();
    QVERIFY(data.value("outputTruncated").toBool());
    QVERIFY(data.value("resetRequired").toBool());
    QVERIFY(data.value("nextToken").isNull());
    const auto complete = host.call("novaterm_read_context", f.identity()).value("data").toObject();
    QVERIFY(!complete.value("recentOutput").toArray().isEmpty());
    arguments = f.identity();
    arguments.insert("sinceToken", complete.value("nextToken"));
    const auto delta = host.call("novaterm_read_context", arguments).value("data").toObject();
    QVERIFY(delta.value("recentOutput").toArray().isEmpty());
}

void McpTests::revokeAndReconnectInvalidateAccess()
{
    Fixture f;
    QVERIFY(f.enable());
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    Host host;
    QVERIFY(host.start(f.runtime(), f.token()));
    const auto old = f.identity();
    QVERIFY(f.service.access().setGrant(f.client, f.service.directory().entries().first(), false));
    QCOMPARE(host.call("novaterm_read_context", old).value("error").toObject().value("code").toString(),
        QStringLiteral("SESSION_NOT_AVAILABLE"));
    QVERIFY(f.service.access().setGrant(f.client, f.service.directory().entries().first(), true));
    f.session.close();
    QVERIFY(f.session.resetForReuse());
    QCOMPARE(host.call("novaterm_read_context", old).value("error").toObject().value("code").toString(),
        QStringLiteral("SESSION_NOT_AVAILABLE"));
}

void McpTests::commandsRequireSeparateGrantAndAreDeduplicated()
{
    Fixture f;
    f.makeSsh();
    QVERIFY(f.enable());
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    Host host;
    QVERIFY(host.start(f.runtime(), f.token()));
    QVERIFY(!host.call("novaterm_list_commands", f.identity()).value("data").toObject().value("executionEnabled").toBool());
    QVERIFY(f.enable(true));
    const auto catalog = host.call("novaterm_list_commands", f.identity()).value("data").toObject();
    QCOMPARE(catalog.value("commands").toArray().size(), 4);
    const auto command = catalog.value("commands").toArray().first().toObject();
    auto arguments = f.identity();
    arguments.insert("commandId", command.value("commandId"));
    arguments.insert("commandTicket", command.value("commandTicket"));
    arguments.insert("policyVersion", catalog.value("policyVersion"));
    arguments.insert("arguments", QJsonObject{{"script", "rm -rf /"}});
    QVERIFY(!host.call("novaterm_execute_command", arguments).value("ok").toBool());
    QCOMPARE(SshTransportTestAccess::queued(*f.ssh), 0);
    arguments.insert("arguments", QJsonObject{});
    const auto permittedId = arguments.value("commandId");
    arguments.insert("commandId", "file.delete");
    QCOMPARE(host.call("novaterm_execute_command", arguments).value("error").toObject().value("code").toString(),
        QStringLiteral("COMMAND_NOT_ALLOWED"));
    arguments.insert("commandId", permittedId);
    host.begin("novaterm_execute_command", arguments);
    QTRY_COMPARE(SshTransportTestAccess::queued(*f.ssh), 1);
    QCOMPARE(SshTransportTestAccess::queuedCommand(*f.ssh),
             QByteArrayLiteral("uname -srm"));
    SshTransportTestAccess::finish(*f.ssh, true);
    const auto completed = host.next().value("result").toObject().value("structuredContent").toObject();
    QVERIFY(completed.value("ok").toBool());
    const auto replay = host.call("novaterm_execute_command", arguments);
    QCOMPARE(replay, completed);
    QCOMPARE(SshTransportTestAccess::queued(*f.ssh), 0);
    QVERIFY(f.service.protectedTargets().isEmpty());
}

void McpTests::unknownExecutionProtectsTarget()
{
    Fixture f;
    f.makeSsh();
    QVERIFY(f.enable(true));
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    Host host;
    QVERIFY(host.start(f.runtime(), f.token()));
    const auto catalog = host.call("novaterm_list_commands", f.identity()).value("data").toObject();
    const auto command = catalog.value("commands").toArray().first().toObject();
    auto arguments = f.identity();
    arguments.insert("commandId", command.value("commandId"));
    arguments.insert("policyVersion", catalog.value("policyVersion"));
    arguments.insert("commandTicket", command.value("commandTicket"));
    arguments.insert("arguments", QJsonObject{});
    host.begin("novaterm_execute_command", arguments);
    QTRY_COMPARE(SshTransportTestAccess::queued(*f.ssh), 1);
    SshTransportTestAccess::finish(*f.ssh, false);
    const auto response = host.next().value("result").toObject().value("structuredContent").toObject();
    QCOMPARE(response.value("error").toObject().value("code").toString(), QStringLiteral("COMMAND_OUTCOME_UNKNOWN"));
    QCOMPARE(f.service.protectedTargets().size(), 1);
    const auto repeated = host.call("novaterm_execute_command", arguments);
    QCOMPARE(repeated, response);
    QCOMPARE(SshTransportTestAccess::queued(*f.ssh), 0);
    const auto nextCatalog = host.call("novaterm_list_commands", f.identity()).value("data").toObject();
    arguments.insert("commandTicket", nextCatalog.value("commands").toArray().first().toObject().value("commandTicket"));
    QCOMPARE(host.call("novaterm_execute_command", arguments).value("error").toObject().value("code").toString(),
        QStringLiteral("COMMAND_EXECUTION_QUARANTINED"));
}

void McpTests::privateMarkersSurviveServiceRestart()
{
    QTemporaryDir root;
    TargetGuard first(root.path() + "/guard");
    const QString target(64, 'a');
    QString error;
    QVERIFY(first.reserve(target, "execution-one", "instance-one", error));
    TargetGuard second(root.path() + "/guard");
    QVERIFY(!second.reserve(target, "execution-two", "instance-two", error));
    QCOMPARE(error, QStringLiteral("COMMAND_EXECUTION_QUARANTINED"));
    QVERIFY(!second.release(target, "wrong-execution"));
    QVERIFY(first.release(target, "execution-one"));
    QVERIFY(second.reserve(target, "execution-two", "instance-two", error));
}

void McpTests::cancelledCommandNeverReplays()
{
    Fixture f;
    f.makeSsh();
    QVERIFY(f.enable(true));
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    Host host;
    QVERIFY(host.start(f.runtime(), f.token()));
    const auto catalog = host.call("novaterm_list_commands", f.identity()).value("data").toObject();
    const auto command = catalog.value("commands").toArray().first().toObject();
    auto arguments = f.identity();
    arguments.insert("commandId", command.value("commandId"));
    arguments.insert("policyVersion", catalog.value("policyVersion"));
    arguments.insert("commandTicket", command.value("commandTicket"));
    arguments.insert("arguments", QJsonObject{});
    const auto request = host.begin("novaterm_execute_command", arguments);
    QTRY_COMPARE(SshTransportTestAccess::queued(*f.ssh), 1);
    host.send({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"},
        {"params", QJsonObject{{"requestId", request}}}});
    QTRY_COMPARE(SshTransportTestAccess::queued(*f.ssh), 0);
    host.send({{"jsonrpc", "2.0"}, {"id", "after-cancel"}, {"method", "ping"}});
    QCOMPARE(host.next().value("id").toString(), QStringLiteral("after-cancel"));
    const auto replay = host.call("novaterm_execute_command", arguments);
    const auto details = replay.value("error").toObject().value("details").toObject();
    QCOMPARE(details.value("status").toString(), QStringLiteral("cancelled"));
    QVERIFY(!details.value("executionMayHaveStarted").toBool());
    QCOMPARE(SshTransportTestAccess::queued(*f.ssh), 0);
    QVERIFY(f.service.protectedTargets().isEmpty());
}

void McpTests::utf8ExpansionIsBounded()
{
    qsizetype budget = 8;
    bool truncated = false;
    const auto invalid = outputText(QByteArray::fromHex(QByteArray(40, 'f')), budget, truncated);
    QVERIFY(truncated);
    QVERIFY(invalid.toUtf8().size() <= 8);
    budget = 16;
    truncated = false;
    QCOMPARE(outputText(QByteArray("\xe4\xb8", 2), budget, truncated), QString(QChar(0xfffd)));
}

void McpTests::duplicateIndexSurvivesEvictionAndReset()
{
    TerminalStateCache cache;
    for (std::size_t i = 0; i <= TerminalStateCache::MaxLines; ++i)
        cache.append(i + 1, "line-" + std::to_string(i));
    QCOMPARE(cache.entries().size(), TerminalStateCache::MaxLines);
    cache.append(2000, "line-0");
    QCOMPARE(cache.entries().back().text, std::string("line-0"));
    QCOMPARE(cache.duplicates(), NovaTerm::u64(0));
    cache.append(2001, "line-0");
    QCOMPARE(cache.duplicates(), NovaTerm::u64(1));
    cache.clear();
    cache.append(3000, "line-0");
    QCOMPARE(cache.entries().size(), std::size_t(1));
    QCOMPARE(cache.duplicates(), NovaTerm::u64(0));
}

void McpTests::settingsDialogSeparatesReadAndScriptPermission()
{
    eApp->init();
    Fixture f;
    f.makeSsh();
    QVERIFY(f.service.access().setEnabled(true));
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    QWidget owner;
    QPointer<McpSettingsDialog> dialog = new McpSettingsDialog(&f.service, &owner);
    dialog->show();
    QTest::qWait(40);
    auto* tree = dialog->findChild<ElaTreeWidget*>();
    QVERIFY(tree && tree->topLevelItemCount() == 1);
    QCOMPARE(tree->columnCount(), 5);
    const auto copyButtons = dialog->findChildren<QPushButton*>();
    QVERIFY(std::any_of(copyButtons.cbegin(), copyButtons.cend(), [](const QPushButton* button) {
        return button->text() == QStringLiteral("Copy for CC Switch");
    }));
    const auto clickPermissionCell = [tree](int column) {
        auto* item = tree->topLevelItem(0);
        if (!item) return false;
        const QRect cell = tree->visualRect(tree->indexFromItem(item, column));
        if (cell.width() <= 30) return false;
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(cell.right() - 4, cell.center().y()));
        return true;
    };
    QVERIFY(clickPermissionCell(2));
    QTRY_VERIFY(f.service.access().canRead(f.client, f.service.directory().entries().first()));
    QVERIFY(f.service.access().commands(f.client, f.service.directory().entries().first()).isEmpty());
    QVERIFY(clickPermissionCell(3));
    QTRY_COMPARE(f.service.access().commands(f.client, f.service.directory().entries().first()).size(), 4);
    QVERIFY(clickPermissionCell(3));
    QTRY_VERIFY(f.service.access().commands(f.client, f.service.directory().entries().first()).isEmpty());
    QTest::qWait(10);
    tree->topLevelItem(0)->setCheckState(3, Qt::Checked);
    QTRY_COMPARE(f.service.access().commands(f.client, f.service.directory().entries().first()).size(), 4);
    QTest::qWait(10);
    tree->topLevelItem(0)->child(0)->setCheckState(3, Qt::Unchecked);
    QTRY_COMPARE(f.service.access().commands(f.client, f.service.directory().entries().first()).size(), 3);
    QTest::qWait(10);
    QVERIFY(!f.service.access().canRunScriptTask(f.client,
        f.service.directory().entries().first()));
    QVERIFY(clickPermissionCell(4));
    QTRY_VERIFY(f.service.access().canRunScriptTask(f.client,
        f.service.directory().entries().first()));
    QVERIFY(clickPermissionCell(4));
    QTRY_VERIFY(!f.service.access().canRunScriptTask(f.client,
        f.service.directory().entries().first()));
    tree->topLevelItem(0)->setCheckState(2, Qt::Unchecked);
    QTRY_VERIFY(!f.service.access().canRead(f.client, f.service.directory().entries().first()));
    QVERIFY(f.service.access().commands(f.client, f.service.directory().entries().first()).isEmpty());
    QTest::qWait(20);
    const auto preview = qEnvironmentVariable("NOVATERM_MCP_UI_PREVIEW");
    if (!preview.isEmpty()) QVERIFY(dialog->grab().save(preview));
    dialog->close();
    QTRY_VERIFY(dialog.isNull());
}

void McpTests::settingsDialogLocalShellFixedDiagnosticsCanBeSelected()
{
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    eApp->init();
    Fixture fixture;
    QVERIFY(fixture.service.access().setEnabled(true));
    QTRY_COMPARE(fixture.service.status(), QStringLiteral("Listening"));
    QWidget owner;
    QPointer<McpSettingsDialog> dialog = new McpSettingsDialog(&fixture.service, &owner);
    dialog->show();
    auto* tree = dialog->findChild<ElaTreeWidget*>();
    QVERIFY(tree && tree->topLevelItemCount() == 1);
    QTRY_COMPARE(tree->topLevelItem(0)->childCount(), 4);
    const auto clickCell = [tree](int column) {
        const QRect cell = tree->visualRect(
            tree->indexFromItem(tree->topLevelItem(0), column));
        if (cell.width() <= 30) return false;
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(cell.right() - 4, cell.center().y()));
        return true;
    };
    QVERIFY(clickCell(2));
    QTRY_VERIFY(fixture.service.access().canRead(
        fixture.client, fixture.service.directory().entries().first()));
    QVERIFY(clickCell(3));
    QTRY_COMPARE(fixture.service.access().commands(
        fixture.client, fixture.service.directory().entries().first()).size(), 4);
    QCOMPARE(tree->topLevelItem(0)->checkState(3), Qt::Checked);
    dialog->close();
    QTRY_VERIFY(dialog.isNull());
#else
    QSKIP("Local fixed diagnostics are not available on this platform.");
#endif
}

void McpTests::stateDirectoryOwnedByTokenDefaultOwnerIsSecurable()
{
    // 提权运行时进程创建的对象属主是令牌的默认属主（Administrators，带
    // SE_GROUP_OWNER），不是 TokenUser。只按 TokenUser 比对属主会让进程无法加固
    // 自己刚创建的状态目录：AccessStore::setEnabled(true) 返回 false，设置界面的
    // “启用本机 MCP 接入”因此表现为点不动（McpSettingsDialog::reportFailure）。
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto directory = root.filePath("state");
    QVERIFY(QDir().mkpath(directory));
    QVERIFY(secureDirectory(directory));
    AccessStore store(directory, {});
    QVERIFY(store.setEnabled(true));
    QVERIFY(store.enabled());
    QVERIFY(QFile::exists(QDir(directory).filePath("access.json")));
    QVERIFY(store.setEnabled(false));
    QVERIFY(!store.enabled());
}

namespace {
bool waitFor(const std::function<bool()>& condition, int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < milliseconds) QTest::qWait(5);
    return condition();
}

int sshLoopbackCheck(quint16 port)
{
    QTemporaryDir root;
    SshConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.username = "fixture";
    config.password = "fixture-only";
    SshTransport transport(config);
    SshTransportTestAccess::knownHosts(transport, root.filePath("known_hosts"));
    QObject::connect(&transport, &SshTransport::hostKeyRequired, &transport, [&transport] { transport.acceptHostKey(); });
    std::optional<CommandExecutionResult> result;
    QObject::connect(&transport, &SshTransport::boundedCommandFinished, &transport,
        [&](const CommandExecutionResult& received) { result = received; });
    if (!transport.connectToHost() || !waitFor([&] { return transport.isConnected(); }, 10000)) {
        std::fprintf(stderr, "Loopback SSH connection failed: %s\n", qPrintable(transport.errorString())); return 1;
    }
    int failures = 0;
    const auto run = [&](quint64 id, const QByteArray& command,
                         CommandExecutionLimits limits,
                         CommandExecutionOutcome outcome) {
        result.reset();
        const bool submitted = transport.executeBoundedCommand(id, command, limits);
        const bool received = waitFor([&] { return result && result->requestId == id; }, 6000);
        const bool ok = submitted && received && result->outcome == outcome
            && result->standardOutput.size() + result->standardError.size() <= limits.maxOutputBytes;
        std::printf("loopback command %llu: %s\n", static_cast<unsigned long long>(id), ok ? "PASS" : "FAIL");
        failures += !ok;
    };
    run(1, "fixture-ok", {64, 1000}, CommandExecutionOutcome::Completed);
    if (!result || !result->terminationConfirmed || result->exitCode != 0) ++failures;
    run(2, "fixture-nonzero", {64, 1000}, CommandExecutionOutcome::Failed);
    if (!result || !result->terminationConfirmed || result->exitCode != 42) ++failures;
    run(3, "fixture-limit", {64, 1000}, CommandExecutionOutcome::OutputLimit);
    if (!result || !result->outputTruncated) ++failures;
    run(4, "fixture-timeout", {64, 200}, CommandExecutionOutcome::TimedOut);
    if (!result || result->terminationConfirmed || !result->executionMayHaveStarted) ++failures;

    SshSessionScriptProvider scriptProvider(&transport);
    QSignalSpy scriptFinished(&scriptProvider,
        &ISessionScriptProvider::finished);
    ScriptWriteRequest scriptRequest;
    scriptRequest.requestId = 51;
    scriptRequest.content = QByteArrayLiteral("#!/bin/sh\nprintf loopback-only\n");
    scriptRequest.targetPath = QStringLiteral("/scripts/novaterm-loopback.sh");
    scriptRequest.workingDirectory = QStringLiteral("/scripts");
    if (!scriptProvider.writeScript(scriptRequest)
        || !waitFor([&] { return scriptFinished.count() == 1; }, 15000)) {
        std::fprintf(stderr, "Loopback SFTP script upload timed out.\n");
        ++failures;
    } else {
        const auto uploaded = qvariant_cast<ScriptWriteResult>(
            scriptFinished.takeFirst().front());
        const bool ok = uploaded.requestId == scriptRequest.requestId
            && uploaded.success
            && uploaded.contentHash == QCryptographicHash::hash(
                scriptRequest.content, QCryptographicHash::Sha256)
            && uploaded.resolvedTargetPath == scriptRequest.targetPath;
        if (!ok) {
            std::fprintf(stderr, "loopback SFTP upload failed (success=%d, error=%s, target=%s)\n",
                int(uploaded.success), qPrintable(uploaded.errorCode),
                qPrintable(uploaded.resolvedTargetPath));
        }
        std::printf("loopback SFTP provider: %s\n", ok ? "PASS" : "FAIL");
        failures += !ok;
    }
    {
        SshTransport interactiveTransport(config);
        SshTransportTestAccess::knownHosts(interactiveTransport,
                                           root.filePath("known_hosts"));
        QObject::connect(&interactiveTransport, &SshTransport::hostKeyRequired,
            &interactiveTransport, [&interactiveTransport] {
                interactiveTransport.acceptHostKey();
            });
        RuntimeConfig runtime;
        runtime.transportKind = TransportKind::Ssh;
        TerminalSession session(runtime);
        session.attach(&interactiveTransport, TerminalSession::Ownership::Borrowed,
                       TransportKind::Ssh);
        const bool connected = session.start()
            && waitFor([&] { return session.state() == SessionState::Running; }, 10000);
        bool commandOk = false;
        if (connected) {
            Service service(root.filePath("state"), root.filePath("instances"),
                            std::make_unique<MemoryCredentialStore>());
            service.directory().add(&session);
            const auto client = service.access().addClient(QStringLiteral("loopback"));
            const bool enabled = !client.isEmpty() && service.access().setEnabled(true)
                && waitFor([&] { return service.status() == QStringLiteral("Listening"); }, 1000);
            const auto entry = service.directory().entries().first();
            if (enabled && service.access().setGrant(client, entry, true)) {
                Host host;
                if (host.start(root.filePath("instances"),
                    service.access().exportToken(client).value_or(QByteArray{}))) {
                    const auto response = host.call("novaterm_run_command",
                        QJsonObject{{"sessionId", entry.id}, {"epoch", entry.epoch},
                            {"command", QStringLiteral("pwd")}, {"timeoutMs", 2000}});
                    commandOk = response.value("ok").toBool()
                        && response.value("data").toObject().value("stdout")
                            .toString().contains(QStringLiteral("/fixture"));
                    if (commandOk && session.core()->waitForIdle()) {
                        QByteArray visible;
                        for (const auto& line : session.core()->terminalState().viewport)
                            visible += QByteArray::fromStdString(line.text);
                        commandOk = visible.contains(QByteArrayLiteral("root# pwd"))
                            && visible.contains(QByteArrayLiteral("/fixture"))
                            && !visible.contains(QByteArrayLiteral("__nvterm"))
                            && !visible.contains(QByteArrayLiteral("NT;END"));
                    } else {
                        commandOk = false;
                    }
                }
            }
        }
        std::printf("loopback interactive pwd: %s\n", commandOk ? "PASS" : "FAIL");
        failures += !commandOk;
        session.close(CloseMode::Abort);
    }
    transport.disconnect();
    std::printf("Loopback SSH result: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

int interopFixture(QCoreApplication& application, const QString& configurationFile)
{
    Fixture fixture;
    fixture.makeSsh();
    if (!fixture.enable(true)) return 1;
    fixture.core.writeInput("SDK_READY\r\nbuild ok\r\nERROR sample\r\n");
    if (!fixture.core.waitForIdle()) return 1;
    QTimer timer;
    timer.setInterval(20);
    bool exported = false;
    QObject::connect(&timer, &QTimer::timeout, &application, [&] {
        if (!exported && fixture.service.status() == "Listening") {
            exported = writePrivateJson(configurationFile, fixture.service.clientConfiguration(fixture.client));
            if (!exported) application.exit(1);
        }
        if (SshTransportTestAccess::queued(*fixture.ssh) > 0) SshTransportTestAccess::finish(*fixture.ssh, true);
        if (QFile::exists(configurationFile + ".stop")) application.quit();
    });
    timer.start();
    QTimer::singleShot(60000, &application, &QCoreApplication::quit);
    return application.exec();
}

int performanceFixture(QCoreApplication& application, const QString& file, bool enabled)
{
    Fixture fixture;
    fixture.core.setScrollbackLimit(1000000);
    if (!fixture.core.waitForIdle()) return 1;
    if (enabled && !fixture.enable()) return 1;
    // 与吞吐同轮测量 CPU 帧时间。渲染器直接订阅 Core 的损伤，不需要
    // TerminalView，因此不引入主题/Ela 接线。注意口径：这里记录的是
    // render() 入口到出口的 CPU 时间，不是端到端 GUI 帧延迟 ——
    // QRhiWidget 没有 frameSwapped 信号，端到端口径当前无法测量。
    TerminalRenderer renderer(&fixture.core);
    bool renderFailed = false;
    QObject::connect(&renderer, &QRhiWidget::renderFailed,
                     [&renderFailed]() { renderFailed = true; });
    renderer.setTargetRefreshRate(60);
    renderer.resize(1152, 760);
    renderer.show();
    // 用真实事件循环等待窗口被窗口管理器映射（与 P3 GPU 基准同一模式；
    // processEvents 的定长轮询不足以让 XCB 的 expose 事件完成投递）。
    {
        QEventLoop exposeLoop;
        QTimer::singleShot(2000, &exposeLoop, &QEventLoop::quit);
        exposeLoop.exec();
    }
    if (renderFailed || !renderer.windowHandle() || !renderer.windowHandle()->isExposed()) {
        QTextStream(stderr)
            << "performance fixture: QRhiWidget 不可用，无法测量 CPU 帧时间。"
               "请在有显示服务的会话中运行，并显式设置 QT_QPA_PLATFORM=xcb "
               "QT_WIDGETS_RHI=1 NOVATERM_RHI_API=opengl（当前 QT_QPA_PLATFORM="
            << qEnvironmentVariable("QT_QPA_PLATFORM", "<未设置>")
            << "，renderFailed=" << (renderFailed ? "true" : "false")
            << "，windowHandle=" << (renderer.windowHandle() ? "有" : "无")
            << "，isExposed="
            << (renderer.windowHandle() && renderer.windowHandle()->isExposed()
                    ? "true" : "false") << "）\n";
        return 3;
    }
    bool bytesOk = false;
    const qint64 configuredBytes = qEnvironmentVariable(
        "NOVATERM_MCP_PERF_BYTES").toLongLong(&bytesOk);
    const qsizetype byteCount = bytesOk
        ? qsizetype(std::clamp<qint64>(configuredBytes, 1024 * 1024,
                                      512LL * 1024 * 1024))
        : qsizetype(64 * 1024 * 1024);
    QByteArray input;
    input.reserve(byteCount + 128);
    for (quint64 index = 0; input.size() < byteCount; ++index)
        input += QByteArray::number(index).rightJustified(8, '0')
            + " build output abcdefghijklmnopqrstuvwxyz 0123456789 abcdefghijklmnop\r\n";
    input.truncate(byteCount);
    QElapsedTimer elapsed;
    qsizetype offset = 0;
    QTimer feed, control;
    feed.setTimerType(Qt::PreciseTimer);
    feed.setInterval(1);
    bool ready = false, started = false, finished = false;
    QObject::connect(&feed, &QTimer::timeout, &application, [&] {
        // 一次最多四块，避免 Windows 定时器粒度成为输入速率上限；满队列立即让出 GUI。
        for (int batch = 0; batch < 4 && offset < input.size(); ++batch) {
            const auto length = std::min(qsizetype(65536), input.size() - offset);
            const auto accepted = fixture.core.writeInput(QByteArrayView(input).sliced(offset, length)).acceptedBytes;
            offset += accepted;
            if (accepted == 0) break;
        }
        if (offset == input.size() && fixture.core.waitForIdle(0)) {
            feed.stop();
            finished = true;
            const double seconds = elapsed.nsecsElapsed() / 1.0e9;
            auto result = fixture.service.statistics();
            result.insert("seconds", seconds);
            result.insert("bytes", double(offset));
            result.insert("throughputMiBps", double(offset) / (1024.0 * 1024.0) / seconds);
            result.insert("mcpEnabled", enabled);
            // 取整轮结束时的快照（与 P3/P5 基准同口径，不对分位数做差）：
            // P50/P95/P99 是渲染器最近 2048 帧的滚动窗口，不是整轮分布。
            const auto frames = renderer.renderStatistics();
            result.insert("framesRendered", double(frames.framesRendered));
            result.insert("cpuFrameP50Ns", double(frames.cpuFrameP50Nanoseconds));
            result.insert("cpuFrameP95Ns", double(frames.cpuFrameP95Nanoseconds));
            result.insert("cpuFrameP99Ns", double(frames.cpuFrameP99Nanoseconds));
            result.insert("cpuFramesOverBudget", double(frames.cpuFramesOverBudget));
            result.insert("lastRenderedRevision", double(frames.lastRenderedRevision));
            // 直接带上 Core 侧发布统计，用于区分「服务端口径」与「解析器真没发布」。
            const auto published = fixture.core.publishedContextStatistics();
            result.insert("coreContextRequestCount", double(published.requestCount));
            result.insert("coreContextPublishCount", double(published.publishCount));
            result.insert("coreContextReuseCount", double(published.reuseCount));
            if (frames.framesRendered == 0) {
                // 零帧必须判为「没画过」，不能当成 0 ms 的极好成绩。
                QTextStream(stderr)
                    << "performance fixture: 负载期间渲染器一帧也没有产出，"
                       "CPU 帧时间无意义（core revision="
                    << double(fixture.core.modelRevision()) << "）\n";
                application.exit(4);
                return;
            }
            if (!writePrivateJson(file + ".result", result)) application.exit(1);
        }
    });
    control.setInterval(20);
    QObject::connect(&control, &QTimer::timeout, &application, [&] {
        if (!ready && (!enabled || fixture.service.status() == "Listening")) {
            ready = writePrivateJson(file, enabled ? fixture.service.clientConfiguration(fixture.client) : QJsonObject{});
            if (!ready) application.exit(1);
        }
        if (!started && ready && QFile::exists(file + ".start")) {
            started = true;
            elapsed.start();
            feed.start();
        }
        if (QFile::exists(file + ".stop")) application.quit();
        if (started && !finished && elapsed.elapsed() > 30000) application.exit(2);
    });
    control.start();
    QTimer::singleShot(60000, &application, &QCoreApplication::quit);
    return application.exec();
}
}

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
#ifdef Q_OS_WIN
    // offscreen 后端不自动读取 Windows 字体目录，显式装入系统字体供布局截图使用。
    const auto fontRoot = qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/";
    if (QFontDatabase::addApplicationFont(fontRoot + "segoeui.ttf") >= 0)
        application.setFont(QFont(QStringLiteral("Segoe UI"), 10));
    QFontDatabase::addApplicationFont(fontRoot + "msyh.ttc");
#endif
    const auto arguments = application.arguments();
    if (arguments.size() == 3 && arguments[1] == "--interop-fixture")
        return interopFixture(application, arguments[2]);
    if (arguments.size() == 3 && (arguments[1] == "--perf-fixture" || arguments[1] == "--perf-baseline"))
        return performanceFixture(application, arguments[2], arguments[1] == "--perf-fixture");
    if (arguments.size() == 3 && arguments[1] == "--ssh-loopback-check") {
        bool ok = false;
        const auto port = arguments[2].toUShort(&ok);
        return ok && port != 0 ? sshLoopbackCheck(port) : 1;
    }
    McpTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "McpTests.moc"
