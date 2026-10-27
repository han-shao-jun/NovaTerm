/** @file McpTests.cpp
 *  @brief MCP 协议、授权、跨客户端摘要及受限命令的离线端到端检查。
 */
#include "mcp/McpService.h"
#include "mcp/application/SavedTargets.h"
#include <QComboBox>
#include <QTreeWidget>
#include "mcp/HistoryCapabilityAdapter.h"
#include "recording/HistoryService.h"
#include "mcp/CommandSessionAdapter.h"
#include "mcp/TerminalClearAdapter.h"
#include "mcp/LogReadAdapter.h"
#include "capabilities/McpCapabilityAdapter.h"
#include "mcp/McpProtocol.h"
#include "mcp/protocol/ToolCodec.h"
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
#include "session/SessionTerminalController.h"
#include "session/ShellIntegration.h"
#include "transport/SshTransport.h"
#include "ui/widgets/McpSettingsDialog.h"
#include "TestGrantSupport.h"
#include "ElaApplication.h"
#include "ElaCheckBox.h"
#include "ElaComboBox.h"
#include "ElaTreeWidget.h"
#include "ElaTheme.h"
#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLockFile>
#include <QProcess>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPersistentModelIndex>
#include <QAbstractItemDelegate>
#include <QMouseEvent>
#include <QStyleOptionViewItem>
#include <QSet>
#include <QPushButton>
#include <QSignalSpy>
#include <QSignalBlocker>
#include <QScopeGuard>
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QTimer>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>

#include <algorithm>
#include <chrono>
#include <tuple>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#endif

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
    TransportCapabilities capabilities() const override {
        return reconnectable ? TransportCapabilities{TransportCapability::Reconnect} : TransportCapabilities{};
    }
    bool online{false}, reconnectable{false};
    QByteArray written;
};

class DelayedScriptProvider final : public ISessionScriptProvider
{
public:
    [[nodiscard]] bool isAvailable() const override { return available; }
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
    void finish(bool success, const QString& errorCode = {})
    {
        const auto request = std::exchange(_request, ScriptWriteRequest{});
        ScriptWriteResult result;
        result.requestId = request.requestId;
        result.success = success;
        result.errorCode = errorCode;
        result.resolvedTargetPath = QDir::cleanPath(QDir::isAbsolutePath(request.targetPath)
            ? request.targetPath : QDir(request.workingDirectory).filePath(request.targetPath));
        result.contentHash = QCryptographicHash::hash(request.content, QCryptographicHash::Sha256);
        emit finished(result);
    }

    int writeCount{0};
    int cancelCount{0};
    bool available{true};

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
        if (beforeCompletion)
            beforeCompletion();
        if (finishSynchronously) {
            emit finished(result);
            return true;
        }
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
    bool finishSynchronously{false};
    std::function<void()> beforeCompletion;
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
        const auto pending = _replies;
        for (auto* reply : pending) {
            QObject::disconnect(reply, nullptr, &_network, nullptr);
            reply->abort();
        }
        if (_session.isEmpty()) return;
        QNetworkRequest request{_endpoint};
        request.setTransferTimeout(std::chrono::seconds(1));
        request.setRawHeader("Authorization", "Bearer " + _token);
        request.setRawHeader("Mcp-Session-Id", _session);
        request.setRawHeader("MCP-Protocol-Version", _protocolVersion.toUtf8());
        auto* reply = _network.deleteResource(request);
        QElapsedTimer deadline;
        deadline.start();
        while (!reply->isFinished() && deadline.elapsed() < 1000) QTest::qWait(1);
        if (!reply->isFinished()) reply->abort();
        reply->deleteLater();
    }
    bool start(const QString& runtime, const QByteArray& token,
               const QString& instance = {}, bool humanForm = false,
               const QString& protocolVersion = ProtocolVersion)
    {
        _protocolVersion = protocolVersion;
        _humanForm = humanForm;
        _token = token;
        QStringList endpoints;
        QElapsedTimer discovery;
        discovery.start();
        // HTTP 监听与 manifest 发布是异步的；明确选择实例时有界等待发布。
        // 多个候选立即失败，由测试调用方显式选择，不能默认挑选其中一个。
        for (;;) {
            endpoints.clear();
            for (const auto& file : QDir(runtime).entryList({"*.json"}, QDir::Files)) {
                const auto manifest = readJson(QDir(runtime).filePath(file), 8192);
                if (!manifest || manifest->value("transport") != "streamable-http") continue;
                if (!instance.isEmpty() && manifest->value("instanceId").toString() != instance) continue;
                endpoints.append(manifest->value("endpoint").toString());
            }
            if (!endpoints.isEmpty() || discovery.elapsed() >= 3000) break;
            QTest::qWait(5);
        }
        if (endpoints.size() != 1) {
            selectionError = endpoints.isEmpty() ? "APP_NOT_RUNNING" : "INSTANCE_SELECTION_REQUIRED";
            return false;
        }
        _endpoint = QUrl(endpoints.first());
        if (!_endpoint.isValid() || _endpoint.scheme() != "http") return false;
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
            QElapsedTimer notified;
            notified.start();
            while (hasPendingRequests() && notified.elapsed() < 3000) QTest::qWait(1);
            if (hasPendingRequests() || lastHttpStatus != 202) return false;
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
            && message.contains("method")) {
            auto params = message.value("params").toObject();
            if (!params.contains("_meta")) params.insert("_meta", requestMeta());
            message.insert("params", params);
        }
        QNetworkRequest request{_endpoint};
        // 覆盖确认、脚本上传及执行三个独立预算，再留少量网络收尾时间。
        request.setTransferTimeout(std::chrono::seconds(220));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("Accept", "application/json, text/event-stream");
        request.setRawHeader("Authorization", "Bearer " + _token);
        if (!_session.isEmpty()) request.setRawHeader("Mcp-Session-Id", _session);
        if (message.value("method") != "initialize") {
            const auto version = message.value("params").toObject().value("_meta").toObject()
                .value("io.modelcontextprotocol/protocolVersion").toString(_protocolVersion);
            request.setRawHeader("MCP-Protocol-Version", version.toUtf8());
        }
        if (_protocolVersion == QStringLiteral("2026-07-28")) {
            request.setRawHeader("Mcp-Method", message.value("method").toString().toUtf8());
            if (message.value("method") == "tools/call")
                request.setRawHeader("Mcp-Name", message.value("params").toObject().value("name").toString().toUtf8());
        }
        auto* reply = _network.post(request, QJsonDocument(message).toJson(QJsonDocument::Compact));
        _replies.insert(reply);
        auto buffer = std::make_shared<QByteArray>();
        const auto collect = [this, reply, buffer] {
            const bool transferFailed = reply->error() != QNetworkReply::NoError
                && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() < 400;
            if (transferFailed) {
                transportError = reply->errorString();
                buffer->clear();
                if (reply->isOpen()) static_cast<void>(reply->readAll());
                return;
            }
            if (reply->isOpen()) buffer->append(reply->readAll());
            if (!reply->rawHeader("Content-Type").startsWith("text/event-stream")) return;
            for (;;) {
                const auto end = buffer->indexOf('\n');
                if (end < 0) break;
                const auto line = buffer->left(end).trimmed();
                buffer->remove(0, end + 1);
                if (!line.startsWith("data:")) continue;
                const auto object = QJsonDocument::fromJson(line.mid(5).trimmed()).object();
                if (!object.isEmpty()) _messages.append(object);
            }
        };
        QObject::connect(reply, &QNetworkReply::readyRead, &_network, collect);
        QObject::connect(reply, &QNetworkReply::finished, &_network, [this, reply, buffer, collect] {
            collect();
            lastHttpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->hasRawHeader("Mcp-Session-Id")) _session = reply->rawHeader("Mcp-Session-Id");
            if (!reply->rawHeader("Content-Type").startsWith("text/event-stream")) {
                const auto object = QJsonDocument::fromJson(*buffer).object();
                if (!object.isEmpty()) _messages.append(object);
            }
            if (reply->error() != QNetworkReply::NoError) transportError = reply->errorString();
            _replies.remove(reply);
            reply->deleteLater();
        });
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
            if (!_messages.isEmpty()) return _messages.takeFirst();
            if (_replies.isEmpty()) return {};
            QTest::qWait(1);
        }
        return {};
    }
    QJsonObject call(const QString& name, const QJsonObject& arguments = {})
    {
        begin(name, arguments);
        return next().value("result").toObject().value("structuredContent").toObject();
    }
    bool hasPendingRequests() const { return !_replies.isEmpty(); }
    void cancelPendingRequests() { const auto replies = _replies; for (auto* reply : replies) reply->abort(); }
    QString selectionError, transportError;
    int lastHttpStatus{0};
    int counter{1};
    QString _protocolVersion{QString::fromLatin1(ProtocolVersion)};
    bool _humanForm{false};
private:
    QNetworkAccessManager _network;
    QSet<QNetworkReply*> _replies;
    QList<QJsonObject> _messages;
    QUrl _endpoint;
    QByteArray _token, _session;
};

struct Fixture
{
    // 显式工作区模板，隔离 AppContainer 默认临时路径；私有权限断言保持。
    QTemporaryDir root{QDir::currentPath() + "/novaterm-mcp-fixture-XXXXXX"};
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
    DelayedScriptProvider* makeScriptUpload()
    {
        session.close(CloseMode::Abort);
        if (!session.resetForReuse())
            return nullptr;
        RuntimeConfig runtime;
        runtime.transportKind = TransportKind::LocalShell;
        runtime.transport.insert(QStringLiteral("interactiveShellKind"), QStringLiteral("posix"));
        session.attach(&local, TerminalSession::Ownership::Borrowed, runtime);
        static_cast<void>(session.start());
        auto provider = std::make_unique<DelayedScriptProvider>();
        auto* delayed = provider.get();
        session.installScriptProvider(std::move(provider));
        return delayed;
    }
    QJsonObject identity()
    {
        const auto entry = service.directory().entries().first();
        return {{"sessionId", entry.id}, {"epoch", entry.epoch}};
    }
    bool enable(bool execute = false, bool interactive = false)
    {
        QSet<QString> commands;
        if (execute) for (const auto& command : CommandPolicy::catalog()) commands.insert(command.id);
        return !client.isEmpty() && service.access().setEnabled(true)
            && NovaTerm::Mcp::TestSupport::setGrant(service.access(), client, service.directory().entries().first(),
                true, commands, interactive);
    }
    QByteArray token() { return service.access().exportToken(client).value_or(QByteArray{}); }
    QString runtime() { return root.path() + "/instances"; }
};

class McpTests final : public QObject
{
    Q_OBJECT
private slots:
    void targetConnectionsUseBothProtocols_data();
    void targetConnectionsUseBothProtocols();
    void targetConnectionQueryFollowsInitialConnectionAndReconnect();
    void targetConnectionNestedRevocationCannotStart();
    void targetCloseUsesOriginalOwnershipAndCurrentSafety_data();
    void targetCloseUsesOriginalOwnershipAndCurrentSafety();
    void workflowWaitAndTasksUseBothProtocols_data();
    void workflowWaitAndTasksUseBothProtocols();
    void workflowWaitCancellationAndMcpReenable_data();
    void workflowWaitCancellationAndMcpReenable();
    void clearTerminalIsExplicitIdempotentAndInvalidatesObservation_data();
    void clearTerminalIsExplicitIdempotentAndInvalidatesObservation();
    void localCacheMutationsInvalidateRetainedObservations_data();
    void localCacheMutationsInvalidateRetainedObservations();
    void expandDistinguishesEvictionFromCacheInvalidation();
    void clearAdapterTracksSessionObjectReuseAndDestruction();
    void clearTicketRejectsHumanTakeoverBeforeAdmission();
    void logReadsPageAndBindClientEpochAndMode_data();
    void logReadsPageAndBindClientEpochAndMode();
    void historyOfflineAccessRequiresExplicitGrant();
    void historyOnlyExplicitPersistentGrantsSurviveAdapterRestart();
    void expiredCapturesDoNotExhaustHistoryGrantCapacity();
    void revokedPendingImportsCannotPublishButOtherClientsCan();
    void localImportsDoNotRequireRecordingSource();
    void historyAdapterBindsTheInjectedSourceGeneration();
    void commandTimeoutKeepsExecutionFacts();
    void terminalInteractionSupportsEveryTransport_data();
    void terminalInteractionSupportsEveryTransport();
    void compactCapturesRetainNewestFour();
    void compactBaseSnapshotsAreRateLimited();
    void elevationAvailabilityUsesIdentityNotConnectionType();
    void terminalInputReportsSequenceAndReleaseAdmission();
    void terminalInteractionRequiresNewGrantAndDeduplicates();
    void compactAttentionWaitCoalescesScrollAndExpires();
    void managedElevationRejectsWithoutAdapter();
    void terminalCommandCompletionAndReadinessAreSeparateFromOutput();
    void compactCaptureExpansionDoesNotMoveLiveCursor();
    void fullProjectionExpansionStartsAtOmittedLine();
    void compactExpansionAdvancesForEscapedLongLine();
    void compactExpansionRejectsTooSmallUtf8Budget();
    void compactCursorSupportsFullProjection();
    void fullProjectionTruncationRequiresReset_data();
    void fullProjectionTruncationRequiresReset();
    void compactExpansionPreservesSourceGap();
    void maximumTerminalCommandIncludesConfiguredLineEnding();
    void tokenSigningAndAuthentication();
    void rejectsDangerousAndOversizedArguments();
    void nonblockingSnapshotPreservesIndependentProgress();
    void httpRequiresRunningApplication();
    void httpConfigurationUsesApplicationEndpoint();
    void httpInitializedSessionReadsWithAuthorization();
    void httpSavedTargetDirectoryIsIsolatedAndVersioned();
    void settingsDialogSharesOnlySelectedSavedTargets();
    void savedTargetSharingSurvivesSessionPermissionChanges();
    void httpRejectsMissingTokenAndForeignOrigin();
    void httpRequestsFailWhenApplicationStops();
    void httpInitializedSessionStopsWithApplication();
    void httpClientReleasesLegacySession();
    void httpClientConnectsAfterApplicationStarts();
    void httpPendingCommandStopsWithApplication();
    void httpModernDisconnectCancelsPendingCommand();
    void readSearchAndClientIsolation();
    void truncatedOutputNeverAdvancesCursor();
    void revokeAndReconnectInvalidateAccess();
    void commandsRequireSeparateGrantAndAreDeduplicated();
    void synchronousCommandCompletionKeepsFinalState();
    void unknownExecutionProtectsTarget();
    void privateMarkersSurviveServiceRestart();
    void cancelledCommandNeverReplays();
    void utf8ExpansionIsBounded();
    void settingsDialogSeparatesReadAndScriptPermission();
    void settingsDialogRoundTripsTerminalPermissions();
    void settingsDialogRoundTripsClearPermission();
    void settingsDialogKeepsPendingGrantsForEveryTerminal_data();
    void settingsDialogKeepsPendingGrantsForEveryTerminal();
    void settingsDialogRejectsStaleConnectionRow_data();
    void settingsDialogRejectsStaleConnectionRow();
    void settingsDialogRejectsRowFromPreviousClient();
    void settingsDialogRejectsElevationWithoutResettingActiveModel_data();
    void settingsDialogRejectsElevationWithoutResettingActiveModel();
    void settingsDialogElevationCheckboxRequiresSupportedContext_data();
    void settingsDialogElevationCheckboxRequiresSupportedContext();
    void settingsDialogLocalShellFixedDiagnosticsCanBeSelected();
    void settingsDialogRefreshPreservesExpandedSessions();
    void settingsDialogBindsTargetReleaseToExecution();
    void settingsDialogUsesTabDisplayTitle();
    void configuredSerialSessionPublishesAndExecutesCommands();
    void serialWithoutPromptCompletesByQuietWindow();
    void accessStoreReportsDistinctSaveFailureReasons();
    void stateDirectoryOwnedByTokenDefaultOwnerIsSecurable();
    void administratorOwnedObjectsMigrateToCurrentUser_data();
    void administratorOwnedObjectsMigrateToCurrentUser();
    void unrelatedGroupOwnedDirectoryIsRejected();
    void settingsDialogCopiesCcSwitchConfiguration();
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
    void readGrantPrewarmsPublishedSnapshotOnlyOnce();
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
    void scriptConfirmationRejectsUnavailableProvider();
    void scriptMrtrConfirmationIsSignedAndOneShot();
    void userTypingCancelsInFlightScriptUploadBeforeInvocation();
    void scriptUploadReservesTargetUntilProviderFinishes_data();
    void scriptUploadReservesTargetUntilProviderFinishes();
    void cancelledScriptUploadRetainsPublicationOutcome_data();
    void cancelledScriptUploadRetainsPublicationOutcome();
    void commandAndScriptProductGrantsAreIndependent();
    void readGrantAloneDoesNotAllowInteractiveCommand();
};

void McpTests::logReadsPageAndBindClientEpochAndMode_data()
{
    QTest::addColumn<QString>("version"); QTest::addColumn<bool>("serial");
    QTest::newRow("2025-ssh") << QString(ProtocolVersion) << false;
    QTest::newRow("2026-ssh") << QString("2026-07-28") << false;
    QTest::newRow("2025-uart") << QString(ProtocolVersion) << true;
    QTest::newRow("2026-uart") << QString("2026-07-28") << true;
}

void McpTests::logReadsPageAndBindClientEpochAndMode()
{
    QFETCH(QString, version); QFETCH(bool, serial);
    Fixture f;
    if (serial) {
        f.session.close(CloseMode::Abort); QVERIFY(f.session.resetForReuse());
        f.session.attach(&f.local, TerminalSession::Ownership::Borrowed, TransportKind::Serial);
        QVERIFY(f.session.start());
    } else f.makeSsh();
    QVERIFY(f.enable());
    f.core.resize(40, 2); QVERIFY(f.core.waitForIdle());
    QVERIFY(f.core.clearAll()); QVERIFY(f.core.waitForIdle());
    Host host; QVERIFY(host.start(f.runtime(), f.token(), {}, false, version));
    auto args = f.identity(); args.insert("source", "log"); args.insert("start", "tail");
    const auto bookmark = host.call("novaterm_read_context", args);
    QVERIFY2(bookmark.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(bookmark).toJson())));
    const auto token = bookmark.value("data").toObject().value("nextToken").toString(); QVERIFY(!token.isEmpty());
    const auto input = QByteArray("same\r\nsame\r\nsame\r\nsame\r\nsame\r\n");
    QVERIFY(f.core.writeInput(input).fullyAccepted()); QVERIFY(f.core.waitForIdle());
    args.remove("start"); args.insert("sinceToken", token); args.insert("maxBytes", 3); args.insert("maxLines", 1);
    QString joined; QJsonObject page;
    QString firstPageToken;
    for (int count = 0; count < 30; ++count) {
        const auto response = host.call("novaterm_read_context", args);
        QVERIFY2(response.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(response).toJson())));
        page = response.value("data").toObject(); QCOMPARE(page.value("status").toString(), QString("ok"));
        const auto fragments = page.value("fragments").toArray(); QVERIFY(fragments.size() <= 1);
        for (const auto& part : fragments) joined += part.toObject().value("text").toString();
        if (firstPageToken.isEmpty()) firstPageToken = page.value("nextToken").toString();
        args.insert("sinceToken", page.value("nextToken"));
        if (page.value("caughtUp").toBool()) break;
    }
    QCOMPARE(joined, QString("same\nsame\nsame\nsame\n"));
    QVERIFY(page.value("caughtUp").toBool()); QVERIFY(!page.value("hasMore").toBool());
    QVERIFY(page.value("historyMayPrecedeEpoch").toBool());
    auto retry = args; retry.insert("sinceToken", token);
    const auto again = host.call("novaterm_read_context", retry).value("data").toObject();
    QCOMPARE(again.value("nextToken").toString(), firstPageToken);
    // Observation tokens never become log positions, and log tokens cannot observe.
    auto observation = f.identity(); observation.insert("responseMode", "compact");
    const auto observed = host.call("novaterm_read_context", observation).value("data").toObject();
    auto wrongMode = args; wrongMode.insert("sinceToken", observed.value("nextToken"));
    QCOMPARE(host.call("novaterm_read_context", wrongMode).value("error").toObject().value("code").toString(), QString("CURSOR_STALE"));
    wrongMode = f.identity(); wrongMode.insert("sinceToken", token);
    QVERIFY(!host.call("novaterm_read_context", wrongMode).value("ok").toBool());
    auto wrongEpoch = args; wrongEpoch.insert("epoch", "different-epoch");
    QCOMPARE(host.call("novaterm_read_context", wrongEpoch).value("error").toObject().value("code").toString(), QString("STALE_SESSION_EPOCH"));
    const auto other = f.service.access().addClient("other");
    const auto entry = f.service.directory().entries().first();
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(f.service.access(), other, entry, true, {}));
    Host second; QVERIFY(second.start(f.runtime(), *f.service.access().exportToken(other), {}, false, version));
    QCOMPARE(second.call("novaterm_read_context", args).value("error").toObject().value("code").toString(), QString("CURSOR_STALE"));
    f.core.clearScrollback(); QVERIFY(f.core.waitForIdle());
    const auto invalid = host.call("novaterm_read_context", args).value("data").toObject();
    QCOMPARE(invalid.value("status").toString(), QString("invalidated"));
    args.insert("sinceToken", invalid.value("nextToken"));
    QCOMPARE(host.call("novaterm_read_context", args).value("data").toObject().value("status").toString(), QString("invalidated"));
    args.insert("sinceToken", invalid.value("earliestToken"));
    QVERIFY(host.call("novaterm_read_context", args).value("data").toObject().value("caughtUp").toBool());
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(f.service.access(), f.client, entry, false, {}));
    QCOMPARE(host.call("novaterm_read_context", args).value("error").toObject().value("code").toString(), QString("SESSION_NOT_AVAILABLE"));
    QVERIFY(f.enable());
    QCOMPARE(host.call("novaterm_read_context", args).value("error").toObject().value("code").toString(), QString("CURSOR_STALE"));
    QVERIFY(f.local.written.isEmpty());
}

void McpTests::historyAdapterBindsTheInjectedSourceGeneration()
{
    Fixture f; const NovaTerm::Recording::HistoryCaptureId capture{QUuid::createUuid()};
    auto resources = std::make_shared<NovaTerm::Resources::ResourceSharingService>(
        [principal = f.client](const QString& id) -> quint64 { return id == principal ? 7 : 0; },
        NovaTerm::Resources::GrantPersistence{{}, [](const auto&) { return true; }});
    resources->setHistorySource({[capture](auto id) -> std::optional<NovaTerm::Recording::HistoryResource> {
        if (id != capture) return {}; NovaTerm::Recording::HistoryResource resource;
        resource.capture.id = capture; resource.generation = 3; return resource;
    }, {}});
    QVERIFY(resources->share(f.client, capture, 1, 0, false).isEmpty());
    HistoryCapabilityAdapter adapter(f.service.access(), resources);
    const auto binding = adapter.binding(HistoryToolRequest{ReadHistoryRequest{HistoryResourceId{capture.text()}, {}, 1}});
    QCOMPARE(std::get<NovaTerm::Capabilities::HistoryCaptureBinding>(binding).generation, quint64(3));
    QVERIFY(f.enable()); QVERIFY(adapter.snapshot(f.client, binding));
}

void McpTests::localImportsDoNotRequireRecordingSource()
{
    Fixture f; QFile file(f.root.path() + "/offline.log"); QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("offline contents"), qint64(16)); file.close();
    std::optional<HistoryCapabilityAdapter::ImportResult> imported;
    f.service.history().importFile(f.client, file.fileName(), [&](auto value) { imported = std::move(value); });
    QTRY_VERIFY(imported.has_value()); QVERIFY2(imported->error.isEmpty(), qPrintable(imported->error));
    QVERIFY(!imported->resourceId.isEmpty());
}

void McpTests::revokedPendingImportsCannotPublishButOtherClientsCan()
{
    Fixture f; NovaTerm::Recording::HistoryService recording(f.root.path() + "/import-revocation");
    QTRY_VERIFY(recording.ready()); f.service.setHistorySource(&recording);
    QFile file(f.root.path() + "/explicit.log"); QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("explicit shared contents"), qint64(24)); file.close();
    const auto other = f.service.access().addClient("other-import-owner"); QVERIFY(!other.isEmpty());
    std::optional<HistoryCapabilityAdapter::ImportResult> revoked, unaffected, reshared;
    auto& adapter = f.service.history();
    adapter.importFile(f.client, file.fileName(), [&](auto result) { revoked = std::move(result); });
    adapter.importFile(other, file.fileName(), [&](auto result) { unaffected = std::move(result); });
    // No event loop turn: even a completed worker cannot have delivered its result yet.
    adapter.clearImportedFiles(f.client);
    QTRY_VERIFY(revoked.has_value() && unaffected.has_value());
    QVERIFY(!revoked->error.isEmpty()); QVERIFY(revoked->resourceId.isEmpty());
    QVERIFY2(unaffected->error.isEmpty(), qPrintable(unaffected->error)); QVERIFY(!unaffected->resourceId.isEmpty());
    adapter.importFile(f.client, file.fileName(), [&](auto result) { reshared = std::move(result); });
    QTRY_VERIFY(reshared.has_value()); QVERIFY2(reshared->error.isEmpty(), qPrintable(reshared->error));
    QVERIFY(!reshared->resourceId.isEmpty());
    f.service.setHistorySource(nullptr);
}

void McpTests::historyOnlyExplicitPersistentGrantsSurviveAdapterRestart()
{
    using namespace NovaTerm::Recording;
    Fixture f;
    HistoryService recording(f.root.path() + "/history-persistent"); QTRY_VERIFY(recording.ready());
    HistoryCaptureId capture; bool started = false;
    QVERIFY(recording.start({f.session.id(), 1}, [&](auto r) { capture = r.captureId; started = r.success; })); QTRY_VERIFY(started);
    QCOMPARE(recording.submit(f.session.id(), 1, "public"), SubmitResult::Accepted);
    QCOMPARE(recording.submit(f.session.id(), 1, "secret"), SubmitResult::Accepted);
    recording.stop({f.session.id(), 1}); QTRY_VERIFY(!recording.captures().front().active);
    const auto other = f.service.access().addClient("temporary-history"); QVERIFY(!other.isEmpty());
    const auto state = f.root.path() + "/separate-history-grants";
    {
        HistoryCapabilityAdapter first(f.service.access(), state); first.setSource(&recording);
        QVERIFY(first.share(f.client, capture, 1, 1, true).isEmpty());
        QVERIFY(first.share(other, capture, 1, 0, false).isEmpty());
    }
    HistoryCapabilityAdapter restored(f.service.access(), state); restored.setSource(&recording);
    QVERIFY(restored.sharingStatus(f.client, capture).contains("persistent"));
    QCOMPARE(restored.sharingStatus(other, capture), QString("Not shared"));
    auto identity = restored.binding(HistoryToolRequest{ReadHistoryRequest{HistoryResourceId{capture.text()}, {}, 1}});
    QVERIFY(restored.accessVersion(f.client, identity) != 0);
    QVERIFY(f.service.access().rotateToken(f.client));
    QCOMPARE(restored.accessVersion(f.client, identity), quint64(0));
}

void McpTests::expiredCapturesDoNotExhaustHistoryGrantCapacity()
{
    using namespace NovaTerm::Recording;
    Fixture f; HistoryService recording(f.root.path() + "/history-capacity"); QTRY_VERIFY(recording.ready());
    HistoryCaptureId capture; bool started = false;
    QVERIFY(recording.start({f.session.id(), 1}, [&](auto r) { capture = r.captureId; started = r.success; })); QTRY_VERIFY(started);
    recording.stop({f.session.id(), 1}); QTRY_VERIFY(!recording.captures().front().active);
    const auto state = f.root.path() + "/history-capacity-grants";
    quint64 tokenEpoch = 0;
    { HistoryCapabilityAdapter adapter(f.service.access(), state);
      tokenEpoch = adapter.accessVersion(f.client, NovaTerm::Capabilities::DirectoryBinding{}); }
    QJsonArray expired;
    HistoryCaptureId firstExpired;
    for (int i = 0; i < 128; ++i) {
        const HistoryCaptureId id{QUuid::createUuid()}; if (!i) firstExpired = id;
        expired.append(QJsonObject{{"principal", f.client}, {"tokenEpoch", QString::number(tokenEpoch)},
            {"captureId", id.text()}, {"generation", "1"}, {"first", "1"}, {"last", "0"}});
    }
    QVERIFY(writePrivateJson(state + "/history-grants.json", {{"version", 1}, {"grants", expired}}));
    HistoryCapabilityAdapter restored(f.service.access(), state); restored.setSource(&recording);
    QVERIFY2(restored.share(f.client, capture, 1, 0, false).isEmpty(), "Deleted capture grants blocked a new explicit share");
    QCOMPARE(restored.sharingStatus(f.client, firstExpired), QString("Not shared"));
}

void McpTests::historyOfflineAccessRequiresExplicitGrant()
{
    using namespace NovaTerm::Recording;
    Fixture f;
    HistoryService recording(f.root.path() + "/history");
    QTRY_VERIFY(recording.ready());
    f.service.setHistorySource(&recording);
    HistoryCaptureId capture;
    QString startError;
    bool started = false;
    QVERIFY(recording.start({f.session.id(), 1}, [&](RecordingResult r) { capture = r.captureId; startError = r.error; started = r.success; }));
    QTRY_VERIFY(capture.valid());
    QVERIFY2(started, qPrintable(startError));
    const auto payload = QByteArray::fromHex("61626300ff6572726f720a") + " a a a a\n";
    QCOMPARE(recording.submit(f.session.id(), 1, payload), SubmitResult::Accepted);
    recording.stop({f.session.id(), 1});
    QTRY_VERIFY(!recording.captures().empty() && !recording.captures().front().active);
    QVERIFY(f.enable());
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    Host host;
    QVERIFY(host.start(f.runtime(), f.token()));
    QCOMPARE(host.call("novaterm_list_captures").value("data").toObject().value("captures").toArray().size(), 0);
    QVERIFY(!host.call("novaterm_read_history", {{"captureId", capture.text()}}).value("ok").toBool());
    QVERIFY2(f.service.history().share(f.client, capture, 1, 0, false).isEmpty(), "Explicit capture share failed");
    f.session.close(CloseMode::Abort);
    QCOMPARE(host.call("novaterm_list_captures").value("data").toObject().value("captures").toArray().size(), 1);
    const auto response = host.call("novaterm_read_history", {{"captureId", capture.text()}});
    QVERIFY2(response.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(response).toJson())));
    QByteArray actual;
    for (const auto& item : response.value("data").toObject().value("records").toArray())
        actual += QByteArray::fromBase64(item.toObject().value("base64").toString().toLatin1());
    QCOMPARE(actual, payload);
    const auto search = host.call("novaterm_search_history", {{"captureId", capture.text()}, {"query", "error"}, {"mode", "text"}});
    QVERIFY2(search.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(search).toJson())));
    const auto hits = search.value("data").toObject().value("matches").toArray();
    QCOMPARE(hits.size(), 1);
    QVERIFY(!hits[0].toObject().value("evidenceId").toString().isEmpty());
    QJsonObject pages{{"captureId", capture.text()}, {"query", "YQ=="}, {"mode", "raw_bytes"}, {"maxMatches", 1}};
    const auto firstPage = host.call("novaterm_search_history", pages);
    const auto firstCursor = firstPage.value("data").toObject().value("nextCursor").toString(); QVERIFY(!firstCursor.isEmpty());
    pages.insert("cursor", firstCursor);
    const auto secondPage = host.call("novaterm_search_history", pages); QVERIFY(secondPage.value("ok").toBool());
    QCOMPARE(host.call("novaterm_search_history", pages), secondPage); // Exact retry keeps the same evidence reference.
    auto changedLimits = pages; changedLimits.insert("maxMatches", 2);
    QCOMPARE(host.call("novaterm_search_history", changedLimits).value("error").toObject().value("code").toString(), QString("CURSOR_MISMATCH"));
    pages.insert("cursor", secondPage.value("data").toObject().value("nextCursor"));
    QVERIFY(host.call("novaterm_search_history", pages).value("ok").toBool());
    pages.insert("cursor", firstCursor);
    QVERIFY(!host.call("novaterm_search_history", pages).value("ok").toBool());
    f.service.history().revoke(f.client, capture);
    QVERIFY(!host.call("novaterm_read_history", {{"captureId", capture.text()}}).value("ok").toBool());
    f.service.setHistorySource(nullptr);
}

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
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(service.access(), client, entry, true, {}, true, true));
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
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(service.access(), client, entry, true, {}, true));
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
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(service.access(), client, entry, true, {}, true));
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
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(service.access(), client, entry, true, {}, true, true));
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
    // 只授「脚本任务」，**不授「交互命令」**：脚本路径必须独立可用。
    // interactiveCommand=false 是本用例的核心前提；若 run_script 反过来依赖
    // 交互命令授权，本例会在这里之后失败（脚本正文已落盘却报
    // COMMAND_PERMISSION_REQUIRED，而不是 SCRIPT_PERMISSION_REQUIRED）。
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(service.access(), client, entry, true, {}, false, false, true));
    QVERIFY(!service.access().canRunCommand(client, entry));
    QVERIFY(service.access().canRunScriptTask(client, entry));
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

void McpTests::scriptConfirmationRejectsUnavailableProvider()
{
    Fixture fixture;
    auto* provider = fixture.makeScriptUpload();
    QVERIFY(provider);
    QVERIFY(fixture.service.access().setEnabled(true));
    const auto entry = fixture.service.directory().entries().first();
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(fixture.service.access(), fixture.client, entry,
        true, {}, true, false, true));
    emit fixture.local.readyRead(QByteArrayLiteral("\x1b]633;NT;PROMPT;1;0\x07"));
    QTRY_VERIFY(fixture.session.commandCoordinator()->isPromptReady());
    QTRY_COMPARE(fixture.service.status(), QStringLiteral("Listening"));
    Host host;
    QVERIFY(host.start(fixture.runtime(), fixture.token(), {}, true));
    const auto target = fixture.root.filePath(QStringLiteral("unavailable.sh"));
    auto arguments = fixture.identity();
    arguments.insert("scriptContent", "#!/bin/sh\nprintf safe\n");
    arguments.insert("targetPath", target);
    arguments.insert("workingDirectory", fixture.root.path());
    arguments.insert("invocation", "sh ./unavailable.sh");
    host.begin("novaterm_run_script", arguments);
    const auto prompt = host.next();
    QCOMPARE(prompt.value("method").toString(), QStringLiteral("elicitation/create"));
    provider->available = false;
    host.send({{"jsonrpc", "2.0"}, {"id", prompt.value("id")},
        {"result", QJsonObject{{"action", "accept"}, {"content", QJsonObject{{"confirmed", true}}}}}});
    const auto result = host.next().value("result").toObject().value("structuredContent").toObject();
    QCOMPARE(result.value("error").toObject().value("code").toString(),
        QStringLiteral("COMMAND_CONFIRMATION_STALE"));
    QCOMPARE(provider->writeCount, 0);
    QVERIFY(fixture.local.written.isEmpty());
    QVERIFY(!QFile::exists(target));
    QVERIFY(fixture.service.protectedTargets().isEmpty());
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
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(service.access(), client, entry, true, {}, true, false, true));
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

    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(service.access(), client, entry, true, {}, true, false, false));
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

    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(service.access(), client, entry, true, {}, true, false, true));
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
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(service.access(), client, entry, true, {}, true, false, true));
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

void McpTests::scriptUploadReservesTargetUntilProviderFinishes_data()
{
    QTest::addColumn<bool>("publishSuccess");
    QTest::newRow("precommit-failure-releases-target") << false;
    QTest::newRow("success-invokes-with-same-reservation") << true;
}

void McpTests::scriptUploadReservesTargetUntilProviderFinishes()
{
    QFETCH(bool, publishSuccess);
    Fixture fixture;
    auto* delayed = fixture.makeScriptUpload();
    QVERIFY(fixture.service.access().setEnabled(true));
    const auto entry = fixture.service.directory().entries().first();
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(fixture.service.access(), fixture.client, entry,
        true, {}, true, false, true));
    emit fixture.local.readyRead(QByteArrayLiteral("\x1b]633;NT;PROMPT;1;0\x07"));
    QTRY_VERIFY(fixture.session.commandCoordinator()->isPromptReady());
    QTRY_COMPARE(fixture.service.status(), QStringLiteral("Listening"));
    Host uploader;
    QVERIFY(uploader.start(fixture.runtime(), fixture.token(), {}, true));
    const QJsonObject arguments{{"sessionId", entry.id}, {"epoch", entry.epoch},
        {"scriptContent", QStringLiteral("#!/bin/sh\nprintf safe\n")},
        {"targetPath", fixture.root.filePath(QStringLiteral("upload.sh"))},
        {"workingDirectory", fixture.root.path()},
        {"invocation", QStringLiteral("sh ./upload.sh")}};
    const int uploadId = uploader.begin("novaterm_run_script", arguments);
    const auto confirmation = uploader.next();
    QCOMPARE(confirmation.value("method").toString(), QStringLiteral("elicitation/create"));
    uploader.send({{"jsonrpc", "2.0"}, {"id", confirmation.value("id")},
        {"result", QJsonObject{{"action", "accept"},
            {"content", QJsonObject{{"confirmed", true}}}}}});
    QTRY_COMPARE(delayed->writeCount, 1);

    const auto markers = fixture.service.protectedTargets();
    QCOMPARE(markers.size(), 1);
    const auto executionId = markers.value(entry.targetFingerprint).toObject()
                                 .value("executionId").toString();
    QVERIFY(!executionId.isEmpty());
    QVERIFY(!fixture.service.acknowledgeTarget(entry.targetFingerprint, executionId));
    Host contender;
    QVERIFY(contender.start(fixture.runtime(), fixture.token(), {}, true));
    auto command = fixture.identity();
    command.insert("command", "uname -srm");
    QCOMPARE(contender.call("novaterm_run_command", command).value("error").toObject()
                 .value("code").toString(), QStringLiteral("COMMAND_EXECUTION_QUARANTINED"));
    const int secondId = contender.begin("novaterm_run_script", arguments);
    auto secondResponse = contender.next();
    if (secondResponse.value("method") == QStringLiteral("elicitation/create")) {
        contender.send({{"jsonrpc", "2.0"}, {"id", secondResponse.value("id")},
            {"result", QJsonObject{{"action", "accept"},
                {"content", QJsonObject{{"confirmed", true}}}}}});
        secondResponse = contender.next();
    }
    QCOMPARE(secondResponse.value("id").toInt(), secondId);
    QCOMPARE(secondResponse.value("result").toObject().value("structuredContent").toObject()
                 .value("error").toObject().value("code").toString(),
             QStringLiteral("COMMAND_EXECUTION_QUARANTINED"));
    QCOMPARE(delayed->writeCount, 1);
    QVERIFY(fixture.local.written.isEmpty());
    QCOMPARE(fixture.service.protectedTargets(), markers);

    if (publishSuccess) {
        delayed->finish(true);
        QTRY_VERIFY(fixture.local.written.contains("sh ./upload.sh"));
        QCOMPARE(fixture.service.protectedTargets(), markers);
        QVERIFY(!fixture.service.acknowledgeTarget(entry.targetFingerprint, executionId));
        emit fixture.local.readyRead(fixture.local.written
            + QByteArrayLiteral("\r\nscript output\r\n\x1b]633;NT;PROMPT;2;0\x07"));
        const auto completed = uploader.next();
        QCOMPARE(completed.value("id").toInt(), uploadId);
        QVERIFY(completed.value("result").toObject().value("structuredContent")
                    .toObject().value("ok").toBool());
    } else {
        delayed->finish(false, QStringLiteral("SCRIPT_WRITE_FAILED"));
        const auto failed = uploader.next();
        QCOMPARE(failed.value("id").toInt(), uploadId);
        QCOMPARE(failed.value("result").toObject().value("structuredContent").toObject()
                     .value("error").toObject().value("code").toString(),
                 QStringLiteral("SCRIPT_WRITE_FAILED"));
    }
    QVERIFY(fixture.service.protectedTargets().isEmpty());
}

void McpTests::cancelledScriptUploadRetainsPublicationOutcome_data()
{
    QTest::addColumn<QString>("cancellation");
    QTest::addColumn<QString>("outcome");
    QTest::newRow("typing-before-publication-failure") << QStringLiteral("typing") << QStringLiteral("failure");
    QTest::newRow("typing-after-publication-success") << QStringLiteral("typing") << QStringLiteral("success");
    QTest::newRow("typing-publication-unknown") << QStringLiteral("typing") << QStringLiteral("unknown");
    QTest::newRow("typing-provider-destroyed") << QStringLiteral("typing") << QStringLiteral("destroyed");
    QTest::newRow("revoked-before-publication-failure") << QStringLiteral("revoked") << QStringLiteral("failure");
    QTest::newRow("revoked-after-publication-success") << QStringLiteral("revoked") << QStringLiteral("success");
    QTest::newRow("disconnected-before-publication-failure") << QStringLiteral("disconnected") << QStringLiteral("failure");
    QTest::newRow("disconnected-after-publication-success") << QStringLiteral("disconnected") << QStringLiteral("success");
}

void McpTests::cancelledScriptUploadRetainsPublicationOutcome()
{
    QFETCH(QString, cancellation);
    QFETCH(QString, outcome);
    Fixture fixture;
    auto* delayed = fixture.makeScriptUpload();
    QVERIFY(fixture.service.access().setEnabled(true));
    const auto entry = fixture.service.directory().entries().first();
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(fixture.service.access(), fixture.client, entry,
        true, {}, true, false, true));
    emit fixture.local.readyRead(QByteArrayLiteral("\x1b]633;NT;PROMPT;1;0\x07"));
    QTRY_VERIFY(fixture.session.commandCoordinator()->isPromptReady());
    QTRY_COMPARE(fixture.service.status(), QStringLiteral("Listening"));
    Host host;
    QVERIFY(host.start(fixture.runtime(), fixture.token(), {}, true));
    const auto target = fixture.root.filePath(QStringLiteral("cancelled.sh"));
    const QJsonObject arguments{{"sessionId", entry.id}, {"epoch", entry.epoch},
        {"scriptContent", QStringLiteral("#!/bin/sh\nprintf private-body\n")},
        {"targetPath", target}, {"workingDirectory", fixture.root.path()},
        {"invocation", QStringLiteral("sh ./cancelled.sh")}};
    const int callId = host.begin("novaterm_run_script", arguments);
    const auto confirmation = host.next();
    QCOMPARE(confirmation.value("method").toString(), QStringLiteral("elicitation/create"));
    host.send({{"jsonrpc", "2.0"}, {"id", confirmation.value("id")},
        {"result", QJsonObject{{"action", "accept"},
            {"content", QJsonObject{{"confirmed", true}}}}}});
    QTRY_COMPARE(delayed->writeCount, 1);
    const auto markers = fixture.service.protectedTargets();
    QCOMPARE(markers.size(), 1);
    const auto executionId = markers.value(entry.targetFingerprint).toObject()
                                 .value("executionId").toString();
    QVERIFY(!executionId.isEmpty());

    if (cancellation == QStringLiteral("typing")) {
        fixture.session.inputArbiter()->submitUserInput(QByteArrayLiteral("user input"));
    } else if (cancellation == QStringLiteral("revoked")) {
        QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(fixture.service.access(), fixture.client, entry,
            true, {}, true, false, false));
    } else {
        fixture.local.disconnect();
    }
    const auto cancelled = host.next();
    QCOMPARE(cancelled.value("id").toInt(), callId);
    QVERIFY(!cancelled.value("result").toObject().value("structuredContent")
                 .toObject().value("ok").toBool());
    QCOMPARE(delayed->cancelCount, 1);
    QCOMPARE(fixture.service.protectedTargets(), markers);
    QVERIFY(!fixture.service.acknowledgeTarget(entry.targetFingerprint, executionId));

    if (outcome == QStringLiteral("destroyed")) {
        fixture.session.installScriptProvider({});
    } else if (outcome == QStringLiteral("success")) {
        delayed->finish(true);
    } else {
        delayed->finish(false, outcome == QStringLiteral("unknown")
            ? QStringLiteral("SCRIPT_PUBLICATION_OUTCOME_UNKNOWN")
            : QStringLiteral("SCRIPT_WRITE_FAILED"));
    }
    if (outcome == QStringLiteral("unknown") || outcome == QStringLiteral("destroyed")) {
        QCOMPARE(fixture.service.protectedTargets(), markers);
    } else {
        QVERIFY(fixture.service.protectedTargets().isEmpty());
    }
    QVERIFY(!fixture.local.written.contains("sh ./cancelled.sh"));
    QVERIFY(!fixture.local.written.contains("private-body"));
    if (outcome != QStringLiteral("failure")) {
        const auto record = [&fixture, executionId] {
            for (const auto& value : fixture.service.executionRecords()) {
                const auto object = value.toObject();
                if (object.value("executionId") == executionId) return object;
            }
            return QJsonObject{};
        };
        QTRY_VERIFY(record().value("complete").toBool());
        const auto error = record().value("result").toObject().value("error").toObject();
        QVERIFY(!error.isEmpty());
        if (outcome == QStringLiteral("success")) {
            QCOMPARE(error.value("code").toString(), QStringLiteral("SCRIPT_PUBLISHED_NOT_INVOKED"));
            const auto details = error.value("details").toObject();
            QVERIFY(details.value("published").toBool());
            QVERIFY(details.contains("invocationStarted"));
            QVERIFY(!details.value("invocationStarted").toBool());
            QCOMPARE(details.value("targetPath").toString(), target);
        } else if (outcome == QStringLiteral("unknown")) {
            QCOMPARE(error.value("code").toString(), QStringLiteral("SCRIPT_PUBLICATION_OUTCOME_UNKNOWN"));
        }
    }
}

void McpTests::commandAndScriptProductGrantsAreIndependent()
{
    Fixture fixture;
    fixture.makeScriptUpload();
    QVERIFY(fixture.enable());
    const auto entry = fixture.service.directory().entries().first();
    emit fixture.local.readyRead(QByteArrayLiteral("\x1b]633;NT;PROMPT;1;0\x07"));
    QTRY_VERIFY(fixture.session.commandCoordinator()->isPromptReady());
    QTRY_COMPARE(fixture.service.status(), QStringLiteral("Listening"));
    Host host;
    QVERIFY(host.start(fixture.runtime(), fixture.token()));
    const auto capabilities = [&host] {
        return host.call("novaterm_list_sessions").value("data").toObject()
            .value("sessions").toArray().first().toObject().value("capabilities").toArray();
    };

    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(fixture.service.access(), fixture.client, entry,
        true, {}, false, false, false));
    QVERIFY(fixture.service.access().canRead(fixture.client, entry));
    QVERIFY(!fixture.service.access().canRunCommand(fixture.client, entry));
    QVERIFY(!fixture.service.access().canRunScriptTask(fixture.client, entry));
    auto advertised = capabilities();
    QVERIFY(!advertised.contains(QStringLiteral("run_command")));
    QVERIFY(!advertised.contains(QStringLiteral("run_script")));

    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(fixture.service.access(), fixture.client, entry,
        true, {}, true, false, false));
    QVERIFY(fixture.service.access().canRunCommand(fixture.client, entry));
    QVERIFY(!fixture.service.access().canRunScriptTask(fixture.client, entry));
    advertised = capabilities();
    QVERIFY(advertised.contains(QStringLiteral("run_command")));
    QVERIFY(!advertised.contains(QStringLiteral("run_script")));

    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(fixture.service.access(), fixture.client, entry,
        true, {}, true, true, false));
    QVERIFY(!fixture.service.access().canRunScriptTask(fixture.client, entry));
    advertised = capabilities();
    QVERIFY(advertised.contains(QStringLiteral("run_command")));
    QVERIFY(!advertised.contains(QStringLiteral("run_script")));

    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(fixture.service.access(), fixture.client, entry,
        true, {}, true, false, true));
    QVERIFY(fixture.service.access().canRunScriptTask(fixture.client, entry));
    advertised = capabilities();
    QVERIFY(advertised.contains(QStringLiteral("run_command")));
    QVERIFY(advertised.contains(QStringLiteral("run_script")));

    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(fixture.service.access(), fixture.client, entry,
        true, {}, false, false, true));
    advertised = capabilities();
    QVERIFY(!advertised.contains(QStringLiteral("run_command")));
    QVERIFY(advertised.contains(QStringLiteral("run_script")));
}

void McpTests::readGrantAloneDoesNotAllowInteractiveCommand()
{
    // 读取共享只允许读。fixture 必须是交互可达的后端（SSH + 无提示符
    // integration），否则 list_sessions 的 interactiveReachable 恒假，
    // capability 断言与授权无关、恒真。
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
    const QString client = service.access().addClient(QStringLiteral("readonly"));
    QVERIFY(!client.isEmpty());
    QVERIFY(service.access().setEnabled(true));
    // 只授予读取共享：固定诊断模板与交互命令都不给。
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(service.access(), client, entry, true));
    QVERIFY(!service.access().canRunCommand(client, entry));
    QTRY_COMPARE(service.status(), QStringLiteral("Listening"));
    Host host;
    QVERIFY(host.start(root.filePath(QStringLiteral("instances")),
        service.access().exportToken(client).value_or(QByteArray{}), {}, true));

    const auto capabilities = [&] {
        return host.call("novaterm_list_sessions")
                   .value("data").toObject()
                   .value("sessions").toArray().first().toObject()
                   .value("capabilities").toArray();
    };
    // 只读授权下不得广告 run_command，也不得写任何字节。
    QVERIFY(!capabilities().contains(QStringLiteral("run_command")));
    const auto denied = host.call("novaterm_run_command",
        QJsonObject{{"sessionId", entry.id}, {"epoch", entry.epoch},
                    {"command", QStringLiteral("pwd")}});
    QCOMPARE(denied.value("error").toObject().value("code").toString(),
             QStringLiteral("COMMAND_PERMISSION_REQUIRED"));
    QVERIFY(transport.written.isEmpty());

    // 单独授予交互命令后：capability 出现，且命令**真的**被写进当前终端 ——
    // 这是权限闸门接线的正面证据，而不是「错误码不等于某个值」这种弱断言。
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(service.access(), client, entry, true, {}, true));
    QVERIFY(service.access().canRunCommand(client, entry));
    QVERIFY(capabilities().contains(QStringLiteral("run_command")));
    const int callId = host.begin("novaterm_run_command",
        QJsonObject{{"sessionId", entry.id}, {"epoch", entry.epoch},
                    {"command", QStringLiteral("pwd")}});
    QTRY_VERIFY_WITH_TIMEOUT(transport.written.startsWith(QByteArrayLiteral("pwd;")), 1000);
    const qsizetype start = transport.written.indexOf(QByteArrayLiteral("NT;END;"));
    const qsizetype stop = transport.written.indexOf(QByteArrayLiteral(";%d"), start);
    QVERIFY(start >= 0 && stop > start + 7);
    const QByteArray nonce = transport.written.mid(start + 7, stop - start - 7);
    emit transport.readyRead(QByteArrayLiteral("root# ") + transport.written
        + QByteArrayLiteral("\n/root\n\x1b]633;NT;END;") + nonce
        + QByteArrayLiteral(";0\x07"));
    const auto response = host.next();
    QCOMPARE(response.value("id").toInt(), callId);
    QVERIFY2(response.value("result").toObject().value("structuredContent")
        .toObject().value("ok").toBool(), qPrintable(QJsonDocument(response).toJson()));

    // 撤销交互命令授权后必须立刻回到权限拒绝，且不再广告该能力。
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(service.access(), client, entry, true, {}, false));
    QVERIFY(!capabilities().contains(QStringLiteral("run_command")));
    const auto reDenied = host.call("novaterm_run_command",
        QJsonObject{{"sessionId", entry.id}, {"epoch", entry.epoch},
                    {"command", QStringLiteral("pwd")}});
    QCOMPARE(reDenied.value("error").toObject().value("code").toString(),
             QStringLiteral("COMMAND_PERMISSION_REQUIRED"));
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
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(service.access(), client, entry, true, {}, true));
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
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(service.access(), client, entry, true, {}, true));
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
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(service.access(), client, entry, true, {}, true));
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
    QVERIFY(fixture.enable(false, true));
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

    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(fixture.service.access(), fixture.client, entry,
                                             true, {}, true));
    QVERIFY(fixture.service.access().canRunCommand(fixture.client, entry));
    QVERIFY(fixture.service.access().commands(fixture.client, entry).isEmpty());
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(fixture.service.access(), fixture.client, entry,
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
    QVERIFY(ToolCodec::validate(QStringLiteral("novaterm_run_command"),
                              base).isEmpty());
    auto invalid = base;
    invalid.insert(QStringLiteral("command"), QString{});
    QVERIFY(!ToolCodec::validate(QStringLiteral("novaterm_run_command"),
                               invalid).isEmpty());
    invalid = base;
    invalid.insert(QStringLiteral("command"), QString(16385, QLatin1Char('x')));
    QVERIFY(!ToolCodec::validate(QStringLiteral("novaterm_run_command"),
                               invalid).isEmpty());
    invalid = base;
    invalid.insert(QStringLiteral("stdin"), QStringLiteral("secret"));
    QVERIFY(!ToolCodec::validate(QStringLiteral("novaterm_run_command"),
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
    // cd 不再是 Allow：run_command 的执行目录是当前 shell 的动态工作目录，免确认的
    // cd 会静默改变此后所有相对路径命令的语义，而它在终端里只是一行普通回显。
    // classify 返回 Unknown，服务层按设计把 Unknown 与 Confirm 同样处理为需确认
    // （McpService::runCommand 的 `risk.decision != Allow` 分支）。
    QCOMPARE(policy.classify(QStringLiteral("cd /tmp")).decision,
             RiskDecision::Unknown);
    QCOMPARE(policy.classify(QStringLiteral("cd")).decision,
             RiskDecision::Unknown);
    QCOMPARE(policy.classify(QStringLiteral("cd ..")).decision,
             RiskDecision::Unknown);
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

void McpTests::readGrantPrewarmsPublishedSnapshotOnlyOnce()
{
    const auto previous = qgetenv("NOVATERM_MCP_PUBLISHED_SNAPSHOT");
    const auto restore = qScopeGuard([previous] {
        if (previous.isNull()) qunsetenv("NOVATERM_MCP_PUBLISHED_SNAPSHOT");
        else qputenv("NOVATERM_MCP_PUBLISHED_SNAPSHOT", previous);
    });
    qputenv("NOVATERM_MCP_PUBLISHED_SNAPSHOT", "1");
    Fixture fixture;
    QVERIFY(fixture.service.access().setEnabled(true));
    const auto entry = fixture.service.directory().entries().first();
    QCOMPARE(entry.state, SessionState::Running);
    const auto before = fixture.core.publishedContextStatistics().requestCount;
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(fixture.service.access(), fixture.client, entry, true));
    QCOMPARE(fixture.core.publishedContextStatistics().requestCount, before + 1);
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(fixture.service.access(), fixture.client, entry, true, {}, true, false, true));
    QCOMPARE(fixture.core.publishedContextStatistics().requestCount, before + 1);
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(fixture.service.access(), fixture.client, entry, true));
    QCOMPARE(fixture.core.publishedContextStatistics().requestCount, before + 1);
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(fixture.service.access(), fixture.client, entry, false));
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(fixture.service.access(), fixture.client, entry, true));
    QCOMPARE(fixture.core.publishedContextStatistics().requestCount, before + 2);
    qputenv("NOVATERM_MCP_PUBLISHED_SNAPSHOT", "0");
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(fixture.service.access(), fixture.client, entry, false));
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(fixture.service.access(), fixture.client, entry, true));
    QCOMPARE(fixture.core.publishedContextStatistics().requestCount, before + 2);
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

    const auto activeTargets = fixture.service.protectedTargets();
    QCOMPARE(activeTargets.size(), 1);
    const auto activeFingerprint = activeTargets.begin().key();
    const auto activeExecutionId = activeTargets.value(activeFingerprint).toObject()
                                       .value("executionId").toString();
    QVERIFY(!activeExecutionId.isEmpty());
    QVERIFY(!fixture.service.acknowledgeTarget(activeFingerprint, activeExecutionId));
    QCOMPARE(fixture.service.protectedTargets(), activeTargets);

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
    const auto executionId = targets.value(fingerprint).toObject().value("executionId").toString();
    QVERIFY(!fixture.service.acknowledgeTarget(fingerprint, QStringLiteral("stale-execution")));
    QCOMPARE(fixture.service.protectedTargets(), targets);
    QVERIFY(fixture.service.acknowledgeTarget(fingerprint, executionId));
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
    QCOMPARE(response.value("data").toObject().value("instanceId").toString(),
        fixture.service.instanceId());
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
    Fixture f;
    QVERIFY(f.enable());
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    Host host;
    QVERIFY(host.start(f.runtime(), f.token()));
    host.send({{"jsonrpc", "2.0"}, {"id", "7"}, {"method", "ping"}});
    host.send({{"jsonrpc", "2.0"}, {"id", 7}, {"method", "ping"}});
    const auto first = host.next().value("id");
    const auto second = host.next().value("id");
    QVERIFY((first == QJsonValue("7") && second == QJsonValue(7))
        || (first == QJsonValue(7) && second == QJsonValue("7")));
    host.send({{"jsonrpc", "2.0"}, {"id", 8}, {"method", "unknown/method"}});
    QCOMPARE(host.next().value("error").toObject().value("code").toInt(), -32601);
    // 原始 HTTP 的 413 另有覆盖。完整上传期间提前拒绝并关闭 TCP，
    // Windows/Qt 也可能将其呈现为连接失败；均不得进入工具调度。
    const auto admitted = f.service.statistics().value("http").toObject().value("acceptedRequests");
    host.send({{"jsonrpc", "2.0"}, {"id", 9}, {"method", "ping"},
        {"params", QJsonObject{{"padding", QString(MaxRequestBytes + 1, QLatin1Char('x'))}}}});
    QTRY_VERIFY_WITH_TIMEOUT(!host.hasPendingRequests(), 5000);
    QVERIFY(host.lastHttpStatus == 413
        || (host.lastHttpStatus == 0 && !host.transportError.isEmpty()));
    QCOMPARE(f.service.statistics().value("http").toObject().value("acceptedRequests"), admitted);
}

void McpTests::instanceSelectionAndDisconnectStayExplicit()
{
    Fixture f;
    QVERIFY(f.enable());
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    const auto other = f.runtime() + "/other.json";
    QVERIFY(writePrivateJson(other, {{"instanceId", newId()}, {"transport", "streamable-http"},
        {"endpoint", "http://127.0.0.1:1/mcp"}}));
    Host ambiguous;
    QVERIFY(!ambiguous.start(f.runtime(), f.token()));
    QCOMPARE(ambiguous.selectionError, QStringLiteral("INSTANCE_SELECTION_REQUIRED"));
    const auto configuration = f.service.clientConfiguration(f.client)
        .value("mcpServers").toObject().value("novaterm").toObject();
    QCOMPARE(configuration.value("url").toString(), f.service.httpEndpoint());
    Host selected;
    QVERIFY(selected.start(f.runtime(), f.token(), f.service.instanceId()));
    QVERIFY(selected.call("novaterm_list_sessions").value("ok").toBool());
    QVERIFY(QFile::remove(other));
    QVERIFY(f.service.access().setEnabled(false));
    QVERIFY(selected.call("novaterm_list_sessions").isEmpty());
    QVERIFY(!selected.transportError.isEmpty());
    QVERIFY(f.enable());
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    // 恢复后显式初始化新 HTTP 会话，不重放旧请求。
    Host replacement;
    QVERIFY(replacement.start(f.runtime(), f.token(), f.service.instanceId()));
    QVERIFY(replacement.call("novaterm_list_sessions").value("ok").toBool());
}

void McpTests::ccSwitchConfigurationIsSingleServerObject()
{
    Fixture f;
    QVERIFY(f.enable());
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    const auto configuration = f.service.ccSwitchConfiguration(f.client);
    QCOMPARE(configuration.value(QStringLiteral("type")).toString(), QStringLiteral("http"));
    QCOMPARE(configuration.value(QStringLiteral("url")).toString(), f.service.httpEndpoint());
    QVERIFY(!configuration.contains(QStringLiteral("command")));
    QVERIFY(!configuration.contains(QStringLiteral("args")));
    QCOMPARE(configuration.value(QStringLiteral("headers")).toObject()
        .value(QStringLiteral("Authorization")).toString(),
        QStringLiteral("Bearer ") + QString::fromLatin1(f.token()));
    QVERIFY(!configuration.contains(QStringLiteral("novaterm")));
    QVERIFY(!configuration.contains(QStringLiteral("mcpServers")));
    QVERIFY(f.service.clientConfiguration(f.client).value(QStringLiteral("mcpServers"))
        .toObject().contains(QStringLiteral("novaterm")));
}

void McpTests::settingsDialogCopiesCcSwitchConfiguration()
{
    eApp->init();
    Fixture f;
    QVERIFY(f.enable());
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    QWidget owner;
    QPointer<McpSettingsDialog> dialog = new McpSettingsDialog(&f.service, &owner);
    const auto closeDialog = qScopeGuard([&] { if (dialog) delete dialog.data(); });
    const auto buttons = dialog->findChildren<QPushButton*>();
    const auto button = std::find_if(buttons.cbegin(), buttons.cend(), [](QPushButton* candidate) {
        return candidate->text() == QStringLiteral("Copy for CC Switch");
    });
    QVERIFY(button != buttons.cend());
    auto* clipboard = QApplication::clipboard();
    const auto previousText = clipboard->text();
    const auto restoreClipboard = qScopeGuard([&] { clipboard->setText(previousText); });
    clipboard->setText(QStringLiteral("not a configuration"));
    (*button)->click();
    const auto configuration = QJsonDocument::fromJson(clipboard->text().toUtf8()).object();
    QCOMPARE(configuration.value("type").toString(), QStringLiteral("http"));
    QCOMPARE(configuration.value("url").toString(), f.service.httpEndpoint());
    QCOMPARE(configuration.value("headers").toObject().value("Authorization").toString(),
        QStringLiteral("Bearer ") + QString::fromLatin1(f.token()));
    QVERIFY(!configuration.contains("mcpServers"));
    QVERIFY(!configuration.contains("novaterm"));
}

void McpTests::tokenSigningAndAuthentication()
{
    const auto secret = randomBytes();
    auto token = signToken({{"scope", "test"}}, secret);
    QVERIFY(verifyToken(token, secret));
    QVERIFY(!verifyToken(token, randomBytes()));
    token[4] = token[4] == 'x' ? 'y' : 'x';
    QVERIFY(!verifyToken(token, secret));
}

void McpTests::rejectsDangerousAndOversizedArguments()
{
    QCOMPARE(ToolCodec::catalog().size(), 21);
    QVERIFY(!CommandPolicy::find("rm"));
    QVERIFY(!CommandPolicy::find("credential.read"));
    const QJsonObject identity{{"sessionId", newId()}, {"epoch", "epoch"}};
    auto arguments = identity;
    arguments.insert("command", "rm -rf /");
    QVERIFY(!ToolCodec::validate("novaterm_read_context", arguments).isEmpty());
    arguments = identity;
    arguments.insert("maxBytes", MaxTextBytes + 1);
    QVERIFY(!ToolCodec::validate("novaterm_read_context", arguments).isEmpty());
    arguments = identity;
    arguments.insert("maxLines", 0.5);
    QVERIFY(!ToolCodec::validate("novaterm_read_context", arguments).isEmpty());
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

void McpTests::httpRequiresRunningApplication()
{
    QTemporaryDir directory;
    Host host;
    QVERIFY(!host.start(directory.path(), {}));
    QCOMPARE(host.selectionError, QStringLiteral("APP_NOT_RUNNING"));
}

namespace {
struct HttpResult {
    int status{0};
    QJsonObject message;
    QByteArray session;
};
HttpResult httpPost(Service& service, QByteArray token, QJsonObject message,
                    QByteArray session = {}, QByteArray origin = {})
{
    QNetworkAccessManager network;
    QNetworkRequest request{QUrl(service.httpEndpoint())};
    request.setTransferTimeout(std::chrono::seconds(5));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json, text/event-stream");
    if (!token.isEmpty()) request.setRawHeader("Authorization", "Bearer " + token);
    if (!session.isEmpty()) {
        request.setRawHeader("Mcp-Session-Id", session);
        request.setRawHeader("MCP-Protocol-Version", ProtocolVersion);
    }
    if (!origin.isEmpty()) request.setRawHeader("Origin", origin);
    auto* reply = network.post(request, QJsonDocument(message).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    if (!reply->isFinished()) loop.exec();
    if (!reply->isFinished()) reply->abort();
    const bool transferFailed = reply->error() != QNetworkReply::NoError
        && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() < 400;
    const auto bytes = !transferFailed && reply->isOpen() ? reply->readAll() : QByteArray{};
    QJsonObject result = QJsonDocument::fromJson(bytes).object();
    if (reply->rawHeader("Content-Type").startsWith("text/event-stream")) {
        for (auto line : bytes.split('\n')) {
            if (line.startsWith("data:")) {
                const auto parsed = QJsonDocument::fromJson(line.mid(5).trimmed()).object();
                if (!parsed.isEmpty()) result = parsed;
            }
        }
    }
    const HttpResult response{reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), result,
        reply->rawHeader("Mcp-Session-Id")};
    reply->deleteLater();
    return response;
}
QJsonObject httpInitialize()
{
    return {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
        {"params", QJsonObject{{"protocolVersion", ProtocolVersion}, {"capabilities", QJsonObject{}},
            {"clientInfo", QJsonObject{{"name", "HTTP fixture"}, {"version", "1"}}}}}};
}
}
void McpTests::httpConfigurationUsesApplicationEndpoint()
{
    Fixture f;
    QVERIFY(f.enable());
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    QVERIFY(!f.service.httpEndpoint().isEmpty());
    const auto config = f.service.ccSwitchConfiguration(f.client);
    QCOMPARE(config.value("type").toString(), QStringLiteral("http"));
    QCOMPARE(config.value("url").toString(), f.service.httpEndpoint());
    QVERIFY(!config.contains("command"));
    QCOMPARE(config.value("headers").toObject().value("Authorization").toString(),
        QStringLiteral("Bearer ") + QString::fromLatin1(f.token()));
}
void McpTests::httpInitializedSessionReadsWithAuthorization()
{
    Fixture f;
    QVERIFY(f.enable());
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    QVERIFY(!f.service.httpEndpoint().isEmpty());
    const auto initialized = httpPost(f.service, f.token(), httpInitialize());
    QCOMPARE(initialized.status, 200);
    QVERIFY(!initialized.session.isEmpty());
    QCOMPARE(initialized.message.value("result").toObject().value("protocolVersion").toString(),
        QString::fromLatin1(ProtocolVersion));
    const auto notification = httpPost(f.service, f.token(),
        {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, initialized.session);
    QCOMPARE(notification.status, 202);
    const auto listed = httpPost(f.service, f.token(),
        {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
         {"params", QJsonObject{{"name", "novaterm_list_sessions"}, {"arguments", QJsonObject{}}}}},
        initialized.session);
    QCOMPARE(listed.status, 200);
    QVERIFY(listed.message.value("result").toObject().value("structuredContent").toObject()
        .value("ok").toBool());
}
void McpTests::targetCloseUsesOriginalOwnershipAndCurrentSafety_data()
{
    QTest::addColumn<QString>("version"); QTest::addColumn<int>("scenario");
    for (const auto& version : QStringList{ProtocolVersion, "2026-07-28"})
        for (int scenario = 0; scenario < 18; ++scenario)
            QTest::newRow(qPrintable(version + "-" + QString::number(scenario))) << version << scenario;
}
void McpTests::targetCloseUsesOriginalOwnershipAndCurrentSafety()
{
    QFETCH(QString, version); QFETCH(int, scenario);
    Fixture f; QVERIFY(f.enable());
    auto& targets = f.service.savedTargets();
    QVERIFY(targets.replace({{newId(), "Board", "serial", "fixture", QByteArray(32, 'a')}}));
    const auto row = targets.localTargets().first();
    const auto identity = f.service.access().identityVersion(f.client);
    QVERIFY(targets.setShared(f.client, identity, row.id, row.configVersion, true));
    QVERIFY(targets.setOpenAllowed(f.client, identity, row.id, row.configVersion, true));
    int opens = 0;
    f.service.setTargetOpener([&](const SavedTargetSource&, const TargetConnections::Guard&) {
        ++opens;
        const auto entry = f.service.directory().find(f.session.id().toString(QUuid::WithoutBraces));
        return TargetConnectionFacts{entry->id, entry->epoch, entry->attachmentId, "running", {}, scenario != 1};
    });
    Host host; QVERIFY(host.start(f.runtime(), f.token(), {}, false, version));
    const QJsonObject args{{"targetId", row.id}, {"configVersion", row.configVersion}, {"operationId", newId()}};
    const auto opened = host.call("novaterm_open_target", args);
    QVERIFY(opened.value("ok").toBool());
    auto closeArgs = args;
    closeArgs.insert("epoch", opened.value("data").toObject().value("epoch"));
    auto* controller = f.session.terminalController();
    InteractiveCommandProfile profile;
    profile.shellIntegration = true;
    profile.allowUnverifiedPrompt = scenario == 14;
    f.session.commandCoordinator()->configure(profile);
    if (scenario != 3) f.session.commandCoordinator()->notePromptReady(7, 0);
    if (scenario == 2) f.session.writeUserInput("human");
    if (scenario == 4) QVERIFY(f.session.inputArbiter()->acquireMcpLease(73, f.session.statistics().generation));
    if (scenario == 5) QVERIFY(f.session.inputArbiter()->acquireTransferLease(73, f.session.statistics().generation,
        [](QByteArrayView bytes) { return bytes.size(); }));
    if (scenario == 6) closeArgs.insert("epoch", "old-epoch");
    if (scenario == 7) QVERIFY(targets.setOpenAllowed(f.client, identity, row.id, row.configVersion, false));
    if (scenario == 0) QVERIFY(controller->readyForCommand());
    if (scenario == 8) f.makeSsh();
    if (scenario >= 9 && scenario <= 11) f.service.setTargetCloseSafety([&](const QString&) {
        if (scenario == 10) static_cast<void>(targets.setOpenAllowed(f.client, identity, row.id, row.configVersion, false));
        if (scenario == 11) f.session.writeUserInput("human during safety");
        return scenario != 9;
    });
    if (scenario == 12 || scenario == 13) {
        f.local.reconnectable = true;
        if (scenario == 12) QVERIFY(f.session.disconnectForReconnect());
        else {
            QVERIFY(f.session.reconnect());
            closeArgs.insert("epoch", host.call("novaterm_query_target", args).value("data").toObject().value("epoch"));
        }
    }
    bool queuedBeforeDelivery = false;
    if (scenario >= 15) f.service.setTargetCloseSafety([&](const QString&) {
        if (scenario == 15) f.core.pasteText("human paste");
        else if (scenario == 16) {
            QKeyEvent key(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            f.core.processKeyPress(&key);
        } else f.core.processTextInput("human IME");
        queuedBeforeDelivery = f.session.inputArbiter()->userInputGeneration() == 0;
        return true;
    });
    const auto stateBeforeClose = f.session.state();
    const auto result = host.call("novaterm_close_target", closeArgs);
    const QStringList errors{"", "TARGET_CLOSE_NOT_ALLOWED", "TARGET_CLOSE_NOT_ALLOWED", "TARGET_CLOSE_BUSY",
        "TARGET_CLOSE_BUSY", "TARGET_CLOSE_BUSY", "STALE_SESSION_EPOCH", "TARGET_OPEN_PERMISSION_REQUIRED",
        "TARGET_CLOSE_NOT_ALLOWED", "TARGET_CLOSE_BUSY", "TARGET_OPEN_PERMISSION_REQUIRED", "TARGET_CLOSE_NOT_ALLOWED",
        "TARGET_CLOSE_NOT_ALLOWED", "TARGET_CLOSE_NOT_ALLOWED", "TARGET_CLOSE_BUSY",
        "TARGET_CLOSE_NOT_ALLOWED", "TARGET_CLOSE_NOT_ALLOWED", "TARGET_CLOSE_NOT_ALLOWED"};
    if (scenario == 0) {
        QVERIFY2(result.value("ok").toBool(), QJsonDocument(result).toJson().constData());
        QCOMPARE(f.session.state(), SessionState::Closed);
        const auto replay = host.call("novaterm_close_target", closeArgs);
        QVERIFY(replay.value("ok").toBool());
        QCOMPARE(replay.value("data"), result.value("data"));
        QCOMPARE(host.call("novaterm_query_target", args).value("data").toObject().value("state").toString(), QString("closed"));
        // A successful close retry must never follow a reset/rebound visible Session.
        QVERIFY(f.session.resetForReuse());
        f.session.attach(&f.local, TerminalSession::Ownership::Borrowed, TransportKind::LocalShell);
        QVERIFY(f.session.start());
        QVERIFY(host.call("novaterm_close_target", closeArgs).value("ok").toBool());
        QCOMPARE(f.session.state(), SessionState::Running);
    } else {
        QCOMPARE(result.value("error").toObject().value("code").toString(), errors[scenario]);
        QCOMPARE(f.session.state(), stateBeforeClose);
    }
    if (scenario >= 15) QVERIFY(queuedBeforeDelivery);
    QCOMPARE(opens, 1);
}

void McpTests::targetConnectionNestedRevocationCannotStart()
{
    Fixture f; QVERIFY(f.enable());
    auto& targets = f.service.savedTargets();
    QVERIFY(targets.replace({{newId(), "Board", "ssh", "fixture.invalid:22", QByteArray(32, 'a')}}));
    const auto row = targets.localTargets().first();
    const auto version = f.service.access().identityVersion(f.client);
    QVERIFY(targets.setShared(f.client, version, row.id, row.configVersion, true));
    QVERIFY(targets.setOpenAllowed(f.client, version, row.id, row.configVersion, true));
    int starts = 0;
    f.service.setTargetOpener([&](const SavedTargetSource&, const TargetConnections::Guard& allowed) {
        QEventLoop nested;
        QTimer::singleShot(0, &nested, [&] {
            static_cast<void>(targets.setOpenAllowed(f.client, version, row.id, row.configVersion, false));
            nested.quit();
        });
        nested.exec(); // Simulates the existing local keyring unlock prompt's event loop.
        if (allowed()) ++starts;
        return TargetConnectionFacts{{}, {}, {}, "unavailable", "admission_invalidated", false};
    });
    Host host; QVERIFY(host.start(f.runtime(), f.token()));
    const QJsonObject args{{"targetId", row.id}, {"configVersion", row.configVersion}, {"operationId", newId()}};
    const auto response = host.call("novaterm_open_target", args);
    QCOMPARE(response.value("error").toObject().value("code").toString(), QString("TARGET_OPEN_PERMISSION_REQUIRED"));
    QCOMPARE(starts, 0);
    QVERIFY(targets.setOpenAllowed(f.client, version, row.id, row.configVersion, true));
    const auto queried = host.call("novaterm_query_target", args);
    QVERIFY(queried.value("ok").toBool());
    QCOMPARE(queried.value("data").toObject().value("reason").toString(), QString("admission_invalidated"));
    QCOMPARE(starts, 0);
}

void McpTests::targetConnectionQueryFollowsInitialConnectionAndReconnect()
{
    class PendingTransport final : public ITransport {
    public:
        bool connectToHost() override { return true; }
        void disconnect() override { online = false; emit disconnected(); }
        void write(const QByteArray&) override {}
        void resizeTerminal(int, int) override {}
        bool isConnected() const override { return online; }
        QString errorString() const override { return {}; }
        TransportCapabilities capabilities() const override { return TransportCapability::Reconnect; }
        bool online{false};
    };
    Fixture f; QVERIFY(f.enable());
    TerminalCore core{80, 24}; PendingTransport transport; TerminalSession session{&core};
    session.attach(&transport, TerminalSession::Ownership::Borrowed, TransportKind::LocalShell);
    QVERIFY(session.start()); QCOMPARE(session.state(), SessionState::Connecting);
    f.service.directory().add(&session);
    auto& targets = f.service.savedTargets();
    QVERIFY(targets.replace({{newId(), "Board", "ssh", "fixture.invalid:22", QByteArray(32, 'a')}}));
    const auto row = targets.localTargets().first();
    const auto version = f.service.access().identityVersion(f.client);
    QVERIFY(targets.setShared(f.client, version, row.id, row.configVersion, true));
    QVERIFY(targets.setOpenAllowed(f.client, version, row.id, row.configVersion, true));
    int opens = 0;
    f.service.setTargetOpener([&](const SavedTargetSource&, const TargetConnections::Guard&) {
        ++opens;
        const auto entry = f.service.directory().find(session.id().toString(QUuid::WithoutBraces));
        return TargetConnectionFacts{entry->id, entry->epoch, entry->attachmentId,
            SessionDirectory::stateName(entry->state), {}, true};
    });
    Host host; QVERIFY(host.start(f.runtime(), f.token()));
    const QJsonObject args{{"targetId", row.id}, {"configVersion", row.configVersion}, {"operationId", newId()}};
    QCOMPARE(host.call("novaterm_open_target", args).value("data").toObject().value("state").toString(), QString("connecting"));
    transport.online = true; emit transport.connected();
    QCOMPARE(session.state(), SessionState::Running);
    const auto connected = host.call("novaterm_query_target", args).value("data").toObject();
    QCOMPARE(connected.value("state").toString(), QString("running"));
    const auto epoch = connected.value("epoch").toString();
    QVERIFY(session.reconnect()); QCOMPARE(session.state(), SessionState::Reconnecting);
    transport.online = true; emit transport.connected();
    const auto reconnected = host.call("novaterm_query_target", args).value("data").toObject();
    QCOMPARE(reconnected.value("state").toString(), QString("running"));
    QVERIFY(reconnected.value("epoch").toString() != epoch); QCOMPARE(opens, 1);
}

void McpTests::targetConnectionsUseBothProtocols_data()
{
    QTest::addColumn<QString>("version");
    QTest::newRow("2025") << QString(ProtocolVersion);
    QTest::newRow("2026") << QString("2026-07-28");
}
void McpTests::targetConnectionsUseBothProtocols()
{
    QFETCH(QString, version);
    Fixture f;
    QVERIFY(f.enable());
    f.makeSsh();
    auto& targets = f.service.savedTargets();
    SavedTargetSource source{newId(), "Board", "ssh", "fixture.invalid:22", QByteArray(32, 'a')};
    QVERIFY(targets.replace({source}));
    const auto row = targets.localTargets().first();
    const auto identityVersion = f.service.access().identityVersion(f.client);
    int opens = 0;
    f.service.setTargetOpener([&](const SavedTargetSource&, const TargetConnections::Guard&) {
        ++opens;
        const auto entry = f.service.directory().find(f.session.id().toString(QUuid::WithoutBraces));
        return TargetConnectionFacts{entry->id, entry->epoch, entry->attachmentId, "running", {}, true};
    });
    Host host;
    QVERIFY(host.start(f.runtime(), f.token(), {}, false, version));
    QJsonObject arguments{{"targetId", row.id}, {"configVersion", row.configVersion}, {"operationId", newId()}};
    const auto denied = [&] {
        return host.call("novaterm_open_target", arguments).value("error").toObject().value("code").toString();
    };
    QCOMPARE(denied(), QString("TARGET_OPEN_PERMISSION_REQUIRED"));
    QVERIFY(targets.setShared(f.client, identityVersion, row.id, row.configVersion, true));
    QCOMPARE(denied(), QString("TARGET_OPEN_PERMISSION_REQUIRED"));
    QCOMPARE(opens, 0);
    McpSettingsDialog dialog(&f.service, nullptr);
    auto* tree = dialog.findChild<QTreeWidget*>("mcpSavedTargets");
    QVERIFY(tree); QCOMPARE(tree->columnCount(), 4);
    QCOMPARE(tree->topLevelItem(0)->checkState(3), Qt::Unchecked);
    tree->topLevelItem(0)->setCheckState(3, Qt::Checked);
    QVERIFY(targets.canOpen(f.client, identityVersion, row.id));
    const auto opened = host.call("novaterm_open_target", arguments);
    QVERIFY2(opened.value("ok").toBool(), QJsonDocument(opened).toJson().constData());
    QVERIFY(opened.value("data").toObject().value("created").toBool());
    const auto sessionId = opened.value("data").toObject().value("sessionId").toString();
    QVERIFY(!sessionId.isEmpty());
    QVERIFY(!f.service.access().canRead(f.client, *f.service.directory().find(sessionId)));
    const auto replay = host.call("novaterm_open_target", arguments);
    QVERIFY(replay.value("ok").toBool()); QCOMPARE(opens, 1);
    QCOMPARE(host.call("novaterm_query_target", arguments).value("data").toObject().value("state").toString(), QString("running"));
    f.ssh->setProperty("novatermHostKeyDecisionPending", true);
    QCOMPARE(host.call("novaterm_query_target", arguments).value("data").toObject().value("reason").toString(),
        QString("host_key_confirmation_required"));
    f.ssh->setProperty("novatermHostKeyDecisionPending", false);
    QCOMPARE(opens, 1);
    // Replacing the transport must never let the old operation follow the new connection.
    f.makeSsh();
    QCOMPARE(host.call("novaterm_query_target", arguments).value("data").toObject().value("state").toString(), QString("invalidated"));
    QCOMPARE(opens, 1);
    QVERIFY(targets.setOpenAllowed(f.client, identityVersion, row.id, row.configVersion, false));
    QCOMPARE(denied(), QString("TARGET_OPEN_PERMISSION_REQUIRED")); QCOMPARE(opens, 1);
    QVERIFY(targets.setOpenAllowed(f.client, identityVersion, row.id, row.configVersion, true));
    QVERIFY(targets.setShared(f.client, identityVersion, row.id, row.configVersion, false));
    QVERIFY(targets.setShared(f.client, identityVersion, row.id, row.configVersion, true));
    QVERIFY(!targets.canOpen(f.client, identityVersion, row.id));
    QCOMPARE(denied(), QString("TARGET_OPEN_PERMISSION_REQUIRED")); QCOMPARE(opens, 1);
}

void McpTests::httpSavedTargetDirectoryIsIsolatedAndVersioned()
{
    Fixture f;
    QVERIFY(f.enable());
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    auto& targets = f.service.savedTargets();
    SavedTargetSource a{newId(), "Board A", "ssh", "fixture.invalid:22", QByteArray(32, 'a')};
    SavedTargetSource b{newId(), "Board B", "serial", "fake-port @ 115200", QByteArray(32, 'b')};
    SavedTargetSource hidden{newId(), "Hidden", "ssh", "private.invalid:22", QByteArray(32, 'c')};
    QVERIFY(targets.replace({a, b, hidden}));
    const auto initializeClient = [&](const QByteArray& token) {
        const auto initialized = httpPost(f.service, token, httpInitialize());
        httpPost(f.service, token, {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, initialized.session);
        return initialized.session;
    };
    const auto session = initializeClient(f.token());
    const auto list = [&](const QByteArray& token, const QByteArray& wireSession, QJsonObject args) {
        return httpPost(f.service, token,
            {{"jsonrpc", "2.0"}, {"id", 44}, {"method", "tools/call"},
             {"params", QJsonObject{{"name", "novaterm_list_targets"}, {"arguments", args}}}}, wireSession)
            .message.value("result").toObject().value("structuredContent").toObject();
    };
    const auto empty = list(f.token(), session, {});
    QVERIFY2(empty.value("ok").toBool(), QJsonDocument(empty).toJson(QJsonDocument::Compact).constData());
    QVERIFY(empty.value("data").toObject().value("targets").toArray().isEmpty());
    for (const auto& row : targets.localTargets())
        if (row.id != hidden.id)
            QVERIFY(targets.setShared(f.client, f.service.access().identityVersion(f.client), row.id, row.configVersion, true));
    const auto first = list(f.token(), session, {{"limit", 1}});
    QVERIFY2(first.value("ok").toBool(), QJsonDocument(first).toJson(QJsonDocument::Compact).constData());
    const auto data = first.value("data").toObject();
    QCOMPARE(data.value("targets").toArray().size(), 1);
    const auto cursor = data.value("nextCursor").toString();
    QVERIFY(!cursor.isEmpty());
    const auto next = list(f.token(), session, {{"cursor", cursor}});
    QVERIFY(next.value("ok").toBool());
    const auto second = next.value("data").toObject().value("targets").toArray();
    QCOMPARE(second.size(), 1);
    QVERIFY(second.first().toObject().value("targetId") != data.value("targets").toArray().first().toObject().value("targetId"));
    const auto bytes = QJsonDocument(list(f.token(), session, {})).toJson();
    QVERIFY(!bytes.contains("private.invalid"));
    QVERIFY(!bytes.contains("configurationKey"));
    QVERIFY(!bytes.contains("credentialRef"));
    QVERIFY(!bytes.contains("sessionId"));
    const auto other = f.service.access().addClient("other");
    const auto otherToken = *f.service.access().exportToken(other);
    const auto otherSession = initializeClient(otherToken);
    QCOMPARE(list(otherToken, otherSession, {{"cursor", cursor}}).value("error").toObject().value("code").toString(), QString("CURSOR_MISMATCH"));
    a.endpoint = "updated.invalid:22";
    QVERIFY(targets.replace({a, b, hidden}));
    QCOMPARE(list(f.token(), session, {{"cursor", cursor}}).value("error").toObject().value("code").toString(), QString("TARGET_DIRECTORY_CHANGED"));
    auto tampered = cursor; tampered[10] = tampered[10] == 'a' ? 'b' : 'a';
    QCOMPARE(list(f.token(), session, {{"cursor", tampered}}).value("error").toObject().value("code").toString(), QString("CURSOR_MISMATCH"));
    QCOMPARE(list(f.token(), session, {}).value("data").toObject().value("targets").toArray().size(), 1);
}

void McpTests::settingsDialogSharesOnlySelectedSavedTargets()
{
    Fixture f;
    auto& targets = f.service.savedTargets();
    QVERIFY(targets.replace({{newId(), "Saved board", "serial", "fake @ 115200", QByteArray(32, 'a')}}));
    const auto other = f.service.access().addClient("other");
    McpSettingsDialog dialog(&f.service, nullptr);
    auto* tree = dialog.findChild<QTreeWidget*>("mcpSavedTargets");
    auto* clients = dialog.findChild<QComboBox*>("mcpClientSelector");
    QVERIFY(tree && clients);
    clients->setCurrentIndex(clients->findData(f.client));
    QCOMPARE(tree->topLevelItemCount(), 1);
    QCOMPARE(tree->topLevelItem(0)->checkState(2), Qt::Unchecked);
    tree->topLevelItem(0)->setCheckState(2, Qt::Checked);
    QCOMPARE(targets.page(f.client, f.service.access().identityVersion(f.client), 25).targets.size(), 1);
    clients->setCurrentIndex(clients->findData(other));
    QCOMPARE(tree->topLevelItem(0)->checkState(2), Qt::Unchecked);
    QVERIFY(targets.page(other, f.service.access().identityVersion(other), 25).targets.isEmpty());
    clients->setCurrentIndex(clients->findData(f.client));
    QCOMPARE(tree->topLevelItem(0)->checkState(2), Qt::Checked);
    QVERIFY(f.service.access().rotateToken(f.client));
    QTRY_VERIFY(tree->topLevelItemCount() == 1 && tree->topLevelItem(0)->checkState(2) == Qt::Unchecked);
    QVERIFY(targets.page(f.client, f.service.access().identityVersion(f.client), 25).targets.isEmpty());
}

void McpTests::savedTargetSharingSurvivesSessionPermissionChanges()
{
    Fixture f;
    QVERIFY(f.enable());
    auto& targets = f.service.savedTargets();
    QVERIFY(targets.replace({{newId(), "Board A", "serial", "fake-a @ 115200", QByteArray(32, 'a')},
        {newId(), "Board B", "ssh", "board.invalid:22", QByteArray(32, 'b')}}));
    McpSettingsDialog dialog(&f.service, nullptr);
    auto* tree = dialog.findChild<QTreeWidget*>("mcpSavedTargets");
    QVERIFY(tree);
    QCOMPARE(tree->topLevelItemCount(), 2);
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        tree->topLevelItem(i)->setCheckState(2, Qt::Checked);
    Host host;
    QVERIFY(host.start(f.runtime(), f.token()));
    const auto first = host.call("novaterm_list_targets", {{"limit", 1}});
    QVERIFY(first.value("ok").toBool());
    const auto cursor = first.value("data").toObject().value("nextCursor").toString();
    QVERIFY(!cursor.isEmpty());
    const auto accessVersion = f.service.access().version(f.client);
    const auto entry = f.service.directory().entries().first();
    for (const auto& change : QList<std::pair<PermissionKind, bool>>{
             {PermissionKind::ReadOutput, false}, {PermissionKind::ReadOutput, true},
             {PermissionKind::FullTerminalInput, true}, {PermissionKind::FullTerminalInput, false}}) {
        QVERIFY(NovaTerm::Mcp::TestSupport::applyOne(f.service.access(), f.client,
            entry, change.first, std::nullopt, change.second));
        const auto listed = host.call("novaterm_list_targets");
        QVERIFY(listed.value("ok").toBool());
        QCOMPARE(listed.value("data").toObject().value("targets").toArray().size(), 2);
        const auto next = host.call("novaterm_list_targets", {{"cursor", cursor}});
        QVERIFY2(next.value("ok").toBool(), QJsonDocument(next).toJson(QJsonDocument::Compact).constData());
        QCOMPARE(next.value("data").toObject().value("targets").toArray().size(), 1);
        QCOMPARE(tree->topLevelItem(0)->checkState(2), Qt::Checked);
        QCOMPARE(tree->topLevelItem(1)->checkState(2), Qt::Checked);
    }
    QVERIFY(f.service.access().version(f.client) > accessVersion);
    const auto previousIdentity = tree->topLevelItem(0)->data(0, Qt::UserRole + 3);
    QVERIFY(f.service.access().rotateToken(f.client));
    QTRY_COMPARE(tree->topLevelItem(0)->checkState(2), Qt::Unchecked);
    Host rotated;
    QVERIFY(rotated.start(f.runtime(), f.token()));
    const auto stale = rotated.call("novaterm_list_targets", {{"cursor", cursor}});
    QCOMPARE(stale.value("error").toObject().value("code").toString(), QString("CURSOR_MISMATCH"));
    tree->topLevelItem(0)->setData(0, Qt::UserRole + 3, previousIdentity);
    tree->topLevelItem(0)->setCheckState(2, Qt::Checked);
    QTRY_COMPARE(tree->topLevelItem(0)->checkState(2), Qt::Unchecked);
    QVERIFY(rotated.call("novaterm_list_targets").value("data").toObject().value("targets").toArray().isEmpty());
    tree->topLevelItem(0)->setCheckState(2, Qt::Checked);
    QCOMPARE(rotated.call("novaterm_list_targets").value("data").toObject().value("targets").toArray().size(), 1);
    QVERIFY(f.service.access().setEnabled(false));
    QVERIFY(f.service.access().setEnabled(true));
    QTRY_COMPARE(tree->topLevelItem(0)->checkState(2), Qt::Unchecked);
    Host resumed;
    QVERIFY(resumed.start(f.runtime(), f.token()));
    QVERIFY(resumed.call("novaterm_list_targets").value("data").toObject().value("targets").toArray().isEmpty());
    tree->topLevelItem(0)->setCheckState(2, Qt::Checked);
    QCOMPARE(resumed.call("novaterm_list_targets").value("data").toObject().value("targets").toArray().size(), 1);
    const auto token = f.token();
    QVERIFY(f.service.access().removeClient(f.client));
    QCOMPARE(f.service.access().authenticate(token), QString());
    QTRY_COMPARE(tree->topLevelItem(0)->checkState(2), Qt::Unchecked);
}

void McpTests::httpRejectsMissingTokenAndForeignOrigin()
{
    Fixture f;
    QVERIFY(f.enable());
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    QVERIFY(!f.service.httpEndpoint().isEmpty());
    QCOMPARE(httpPost(f.service, {}, httpInitialize()).status, 401);
    QCOMPARE(httpPost(f.service, f.token(), httpInitialize(), {}, "http://evil.invalid").status, 403);
}

void McpTests::httpRequestsFailWhenApplicationStops()
{
    Fixture f;
    QVERIFY(f.enable());
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    Host host;
    QVERIFY(host.start(f.runtime(), f.token()));
    QVERIFY(host.call("novaterm_list_sessions").value("ok").toBool());
    f.service.stop();
    QVERIFY(host.call("novaterm_list_sessions").isEmpty());
    QVERIFY(!host.transportError.isEmpty());
}

void McpTests::httpInitializedSessionStopsWithApplication()
{
    Fixture f;
    QVERIFY(f.enable());
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    Host host;
    QVERIFY(host.start(f.runtime(), f.token()));
    f.service.stop();
    host.send({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}});
    QVERIFY(host.next().isEmpty());
    QVERIFY(!host.transportError.isEmpty());
}

void McpTests::httpClientReleasesLegacySession()
{
    Fixture f;
    QVERIFY(f.enable());
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    QCOMPARE(f.service.statistics().value("activeClients").toInt(), 0);
    {
        Host host;
        QVERIFY(host.start(f.runtime(), f.token()));
        QCOMPARE(f.service.statistics().value("activeClients").toInt(), 1);
    }
    // HTTP socket 断开不足以结束 2025 逻辑会话；客户端析构应 DELETE。
    QTRY_COMPARE(f.service.statistics().value("activeClients").toInt(), 0);
}

void McpTests::httpClientConnectsAfterApplicationStarts()
{
    Fixture f;
    Host offline;
    QVERIFY(!offline.start(f.runtime(), f.token()));
    QCOMPARE(offline.selectionError, QStringLiteral("APP_NOT_RUNNING"));
    QVERIFY(f.enable());
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    Host host;
    QVERIFY(host.start(f.runtime(), f.token()));
    QVERIFY(host.call("novaterm_list_sessions").value("ok").toBool());
}

void McpTests::httpPendingCommandStopsWithApplication()
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
    const auto requestId = host.begin("novaterm_execute_command", arguments);
    QTRY_COMPARE(SshTransportTestAccess::queued(*f.ssh), 1);
    f.service.stop();
    const auto response = host.next();
    if (!response.isEmpty()) {
        QCOMPARE(response.value("id"), QJsonValue(requestId));
        QVERIFY(!response.value("result").toObject().value("structuredContent").toObject()
            .value("ok").toBool());
    } else {
        // 断流只能作为传输失败，客户端不伪造命令执行结果或自动重放。
        QVERIFY(!host.transportError.isEmpty());
    }
    QTRY_VERIFY_WITH_TIMEOUT(!host.hasPendingRequests(), 3000);
    QCOMPARE(SshTransportTestAccess::queued(*f.ssh), 0);
}

void McpTests::httpModernDisconnectCancelsPendingCommand()
{
    Fixture f;
    f.makeSsh();
    QVERIFY(f.enable(true));
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    Host host;
    QVERIFY(host.start(f.runtime(), f.token(), {}, false, QStringLiteral("2026-07-28")));
    const auto catalog = host.call("novaterm_list_commands", f.identity()).value("data").toObject();
    const auto command = catalog.value("commands").toArray().first().toObject();
    auto arguments = f.identity();
    arguments.insert("commandId", command.value("commandId"));
    arguments.insert("policyVersion", catalog.value("policyVersion"));
    arguments.insert("commandTicket", command.value("commandTicket"));
    arguments.insert("arguments", QJsonObject{});
    host.begin("novaterm_execute_command", arguments);
    QTRY_COMPARE(SshTransportTestAccess::queued(*f.ssh), 1);
    // 2026 请求的 HTTP SSE 断流必须取消底层任务，不重放执行请求。
    host.cancelPendingRequests();
    QTRY_COMPARE(SshTransportTestAccess::queued(*f.ssh), 0);
    QTRY_VERIFY(!host.hasPendingRequests());
    QVERIFY(!host.transportError.isEmpty());
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
    QVERIFY(!invalid.start(f.runtime(), QByteArray(64, 'z')));
    QCOMPARE(invalid.lastHttpStatus, 401);
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
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(f.service.access(), f.client, f.service.directory().entries().first(), false));
    QCOMPARE(host.call("novaterm_read_context", old).value("error").toObject().value("code").toString(),
        QStringLiteral("SESSION_NOT_AVAILABLE"));
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(f.service.access(), f.client, f.service.directory().entries().first(), true));
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
    QCOMPARE(f.service.executionRecords().first().toObject()
                 .value("phase").toString(), QStringLiteral("observing"));
    QCOMPARE(SshTransportTestAccess::queuedCommand(*f.ssh),
             QByteArrayLiteral("uname -srm"));
    SshTransportTestAccess::finish(*f.ssh, true);
    const auto completed = host.next().value("result").toObject().value("structuredContent").toObject();
    QVERIFY(completed.value("ok").toBool());
    const auto replay = host.call("novaterm_execute_command", arguments);
    QCOMPARE(replay, completed);
    QCOMPARE(f.service.executionRecords().first().toObject()
                 .value("phase").toString(), QStringLiteral("final"));
    QCOMPARE(SshTransportTestAccess::queued(*f.ssh), 0);
    QVERIFY(f.service.protectedTargets().isEmpty());
}

void McpTests::synchronousCommandCompletionKeepsFinalState()
{
    Fixture fixture;
    auto executor = std::make_unique<TestCommandExecutor>();
    auto* executorView = executor.get();
    executorView->finishSynchronously = true;
    QString submittedPhase;
    bool reservedBeforeSubmission = false;
    executorView->beforeCompletion = [&] {
        submittedPhase = fixture.service.executionRecords().first().toObject().value("phase").toString();
        reservedBeforeSubmission = !fixture.service.protectedTargets().isEmpty();
    };
    fixture.session.commandFacade()->installExecutor(std::move(executor), fixture.session.statistics().generation);
    static_cast<void>(fixture.service.directory().entries());
    QVERIFY(fixture.enable(true));
    QTRY_COMPARE(fixture.service.status(), QStringLiteral("Listening"));
    Host host;
    QVERIFY(host.start(fixture.runtime(), fixture.token()));
    const auto catalog = host.call("novaterm_list_commands", fixture.identity()).value("data").toObject();
    const auto command = catalog.value("commands").toArray().first().toObject();
    auto arguments = fixture.identity();
    arguments.insert("commandId", command.value("commandId"));
    arguments.insert("commandTicket", command.value("commandTicket"));
    arguments.insert("policyVersion", catalog.value("policyVersion"));
    arguments.insert("arguments", QJsonObject{});
    const auto completed = host.call("novaterm_execute_command", arguments);
    QVERIFY(completed.value("ok").toBool());
    QCOMPARE(submittedPhase, QStringLiteral("submitted"));
    QVERIFY(reservedBeforeSubmission);
    QCOMPARE(fixture.service.executionRecords().size(), 1);
    QCOMPARE(fixture.service.executionRecords().first().toObject().value("phase").toString(), QStringLiteral("final"));
    QVERIFY(fixture.service.protectedTargets().isEmpty());
    QCOMPARE(host.call("novaterm_execute_command", arguments), completed);
    QCOMPARE(executorView->submissionCount, 1);
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
    QCOMPARE(details.value("instanceId").toString(), f.service.instanceId());
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
    // 原六列加完整输入、托管提权、凭据和当前操作四列。
    QCOMPARE(tree->columnCount(), 11);
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
    // 交互命令列独立于脚本列：此刻脚本列尚未勾选，勾上交互命令不应顺带打开它。
    QVERIFY(!f.service.access().canRunScriptTask(f.client,
        f.service.directory().entries().first()));
    QVERIFY(clickPermissionCell(5));
    QTRY_VERIFY(f.service.access().canRunCommand(f.client,
        f.service.directory().entries().first()));
    QVERIFY(!f.service.access().canRunScriptTask(f.client,
        f.service.directory().entries().first()));
    QVERIFY(clickPermissionCell(5));
    QTRY_VERIFY(!f.service.access().canRunCommand(f.client,
        f.service.directory().entries().first()));
    // 反向：勾上脚本列不应顺带打开交互命令。
    QVERIFY(clickPermissionCell(4));
    QTRY_VERIFY(f.service.access().canRunScriptTask(f.client,
        f.service.directory().entries().first()));
    QVERIFY(!f.service.access().canRunCommand(f.client,
        f.service.directory().entries().first()));
    QVERIFY(clickPermissionCell(4));
    QTRY_VERIFY(!f.service.access().canRunScriptTask(f.client,
        f.service.directory().entries().first()));
    // 取消读取共享的级联必须在两项执行类授权都处于开启时验证，否则断言恒成立。
    QVERIFY(clickPermissionCell(4));
    QVERIFY(clickPermissionCell(5));
    QTRY_VERIFY(f.service.access().canRunScriptTask(f.client,
        f.service.directory().entries().first()));
    QTRY_VERIFY(f.service.access().canRunCommand(f.client,
        f.service.directory().entries().first()));
    QVERIFY2(tree->topLevelItem(0)->checkState(4) == Qt::Checked
        && tree->topLevelItem(0)->checkState(5) == Qt::Checked,
        "precondition: both execution grants are checked before revoking read");
    tree->topLevelItem(0)->setCheckState(2, Qt::Unchecked);
    QTRY_VERIFY(!f.service.access().canRead(f.client, f.service.directory().entries().first()));
    QVERIFY(f.service.access().commands(f.client, f.service.directory().entries().first()).isEmpty());
    // 取消读取共享时，执行类授权必须一起被清掉。
    QVERIFY(!f.service.access().canRunScriptTask(f.client,
        f.service.directory().entries().first()));
    QVERIFY(!f.service.access().canRunCommand(f.client,
        f.service.directory().entries().first()));
    // 且复选框在界面上确实被清掉，而不只是存储层返回 false。
    QCOMPARE(tree->topLevelItem(0)->checkState(4), Qt::Unchecked);
    QCOMPARE(tree->topLevelItem(0)->checkState(5), Qt::Unchecked);
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

void McpTests::configuredSerialSessionPublishesAndExecutesCommands()
{
    QTemporaryDir root;
    TerminalCore core(80, 24);
    TerminalSession session(&core);
    LocalFake transport;
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Serial;
    runtime.transport = {
        {QStringLiteral("portName"), QStringLiteral("fake-serial-only")},
        {QStringLiteral("interactivePromptPattern"), QStringLiteral("^device> $")},
        {QStringLiteral("interactiveLineEnding"), QStringLiteral("crlf")},
        {QStringLiteral("interactiveRemoteEcho"), false}};
    session.attach(&transport, TerminalSession::Ownership::Borrowed, runtime);
    QVERIFY(session.start());
    Service service(root.path() + "/state", root.path() + "/instances",
                    std::make_unique<MemoryCredentialStore>());
    service.directory().add(&session);
    const auto entry = service.directory().entries().first();
    const auto client = service.access().addClient(QStringLiteral("serial-test"));
    QVERIFY(service.access().setEnabled(true));
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(service.access(), client, entry, true, {}, true));
    QTRY_COMPARE(service.status(), QStringLiteral("Listening"));
    Host host;
    QVERIFY(host.start(root.path() + "/instances",
                       service.access().exportToken(client).value_or(QByteArray{})));
    const auto notReady = host.call("novaterm_run_command",
        QJsonObject{{"sessionId", entry.id}, {"epoch", entry.epoch}, {"command", "uname -srm"}});
    QCOMPARE(notReady.value("error").toObject().value("code").toString(),
             QStringLiteral("SESSION_COMMAND_NOT_READY"));
    QVERIFY(transport.written.isEmpty());
    emit transport.readyRead(QByteArrayLiteral("device> "));
    QTRY_VERIFY_WITH_TIMEOUT(session.commandCoordinator()->isPromptReady(), 500);
    const auto catalog = host.call("novaterm_list_sessions").value("data").toObject()
        .value("sessions").toArray().first().toObject().value("capabilities").toArray();
    QVERIFY(catalog.contains(QStringLiteral("run_command")));
    QVERIFY(!catalog.contains(QStringLiteral("run_script")));
    const int callId = host.begin("novaterm_run_command",
        QJsonObject{{"sessionId", entry.id}, {"epoch", entry.epoch}, {"command", "uname -srm"}});
    QTRY_COMPARE(transport.written, QByteArrayLiteral("uname -srm\r\n"));
    emit transport.readyRead(QByteArrayLiteral("Linux test\r\ndevice> "));
    const auto reply = host.next();
    QCOMPARE(reply.value("id").toInt(), callId);
    const auto payload = reply.value("result").toObject().value("structuredContent").toObject();
    QVERIFY(payload.value("ok").toBool());
    QVERIFY(payload.value("data").toObject().value("stdout").toString().contains(QStringLiteral("Linux test")));
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(service.access(), client, entry, true));
    const auto revoked = host.call("novaterm_run_command",
        QJsonObject{{"sessionId", entry.id}, {"epoch", entry.epoch}, {"command", "uname -srm"}});
    QCOMPARE(revoked.value("error").toObject().value("code").toString(),
             QStringLiteral("COMMAND_PERMISSION_REQUIRED"));
    QCOMPARE(transport.written, QByteArrayLiteral("uname -srm\r\n"));
}

void McpTests::serialWithoutPromptCompletesByQuietWindow()
{
    QTemporaryDir root;
    TerminalCore core(80, 24);
    TerminalSession session(&core);
    LocalFake transport;
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Serial;
    runtime.transport = {
        {QStringLiteral("portName"), QStringLiteral("fake-serial-idle")}};
    session.attach(&transport, TerminalSession::Ownership::Borrowed, runtime);
    QVERIFY(session.start());
    // 空规则降级为空闲收尾；单测缩短静默窗，默认 3000ms 由 Profile 用例断言。
    auto idleProfile = *ShellIntegration::profileFor(runtime);
    QVERIFY(idleProfile.allowIdleCompletion);
    idleProfile.idleQuietMs = 80;
    session.commandCoordinator()->configure(idleProfile);

    Service service(root.path() + "/state", root.path() + "/instances",
                    std::make_unique<MemoryCredentialStore>());
    service.directory().add(&session);
    const auto entry = service.directory().entries().first();
    const auto client = service.access().addClient(QStringLiteral("serial-idle-test"));
    QVERIFY(service.access().setEnabled(true));
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(service.access(), client, entry, true, {}, true));
    QTRY_COMPARE(service.status(), QStringLiteral("Listening"));
    Host host;
    QVERIFY(host.start(root.path() + "/instances",
                       service.access().exportToken(client).value_or(QByteArray{})));

    const auto catalog = host.call("novaterm_list_sessions").value("data").toObject()
        .value("sessions").toArray().first().toObject().value("capabilities").toArray();
    QVERIFY(catalog.contains(QStringLiteral("run_command")));
    QVERIFY(!catalog.contains(QStringLiteral("run_script")));

    // 没有提示符就绪步骤：命令直发，静默窗收口为"完成未验证"。
    const int callId = host.begin("novaterm_run_command",
        QJsonObject{{"sessionId", entry.id}, {"epoch", entry.epoch}, {"command", "uname -srm"}});
    QTRY_COMPARE(transport.written, QByteArrayLiteral("uname -srm\r"));
    emit transport.readyRead(QByteArrayLiteral("uname -srm\r\nLinux test\r\n"));
    const auto reply = host.next();
    QCOMPARE(reply.value("id").toInt(), callId);
    const auto payload = reply.value("result").toObject().value("structuredContent").toObject();
    QVERIFY(payload.value("ok").toBool());
    const auto data = payload.value("data").toObject();
    QCOMPARE(data.value("status").toString(), QStringLiteral("completed"));
    QVERIFY(data.value("terminationConfirmed").toBool());
    QVERIFY(data.value("exitCode").isNull());
    const auto stdoutText = data.value("stdout").toString();
    QVERIFY(stdoutText.contains(QStringLiteral("Linux test")));
    QVERIFY(!stdoutText.contains(QStringLiteral("uname -srm")));
}

void McpTests::settingsDialogUsesTabDisplayTitle()
{
    eApp->init();
    Fixture f;
    auto& directory = f.service.directory();
    directory.setDisplayTitle(&f.session, QStringLiteral("本地终端"));
    const auto before = directory.entries().first();
    QWidget owner;
    QPointer<McpSettingsDialog> dialog = new McpSettingsDialog(&f.service, &owner);
    dialog->show();
    auto* tree = dialog->findChild<ElaTreeWidget*>();
    QVERIFY(tree && tree->topLevelItemCount() == 1);
    QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("本地终端"));

    // Shell 的 OSC 标题不应覆盖标签名称，未上报标题也不再只显示 UUID。
    emit f.session.titleChanged(QStringLiteral("super@rocket: ~/Documents"));
    QCOMPARE(directory.entries().first().title, QStringLiteral("本地终端"));
    directory.setDisplayTitle(&f.session, QStringLiteral("开发终端"));
    QTRY_COMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("开发终端"));
    QVERIFY(tree->topLevelItem(0)->toolTip(0).contains(before.id));
    const auto after = directory.entries().first();
    QCOMPARE(after.id, before.id);
    QCOMPARE(after.epoch, before.epoch);
    QCOMPARE(after.attachmentId, before.attachmentId);

    // 相同名称不反复触发刷新；目录仍保留既有的标题长度上限。
    const auto revision = directory.revision();
    directory.setDisplayTitle(&f.session, after.title);
    QCOMPARE(directory.revision(), revision);
    directory.setDisplayTitle(&f.session, QString(2048, QLatin1Char('x')));
    QCOMPARE(directory.entries().first().title.size(), 1024);
    dialog->close();
    QTRY_VERIFY(dialog.isNull());
}

void McpTests::settingsDialogBindsTargetReleaseToExecution()
{
    eApp->init();
    Fixture fixture;
    TargetGuard guard(fixture.root.path() + "/state/targets");
    const QString first(64, QLatin1Char('a'));
    const QString second(64, QLatin1Char('b'));
    QString error;
    QVERIFY(guard.reserve(first, "execution-first", "test-instance", error));
    QVERIFY(guard.reserve(second, "execution-second", "test-instance", error));
    QWidget owner;
    QPointer<McpSettingsDialog> dialog = new McpSettingsDialog(&fixture.service, &owner);
    ElaComboBox* targets{nullptr};
    for (auto* combo : dialog->findChildren<ElaComboBox*>()) {
        if (combo->findData(second) >= 0) targets = combo;
    }
    QVERIFY(targets);
    ElaCheckBox* confirmed{nullptr};
    for (auto* checkbox : dialog->findChildren<ElaCheckBox*>()) {
        if (checkbox->text() == QStringLiteral("I have checked that the remote command has stopped"))
            confirmed = checkbox;
    }
    QVERIFY(confirmed);
    QPushButton* release{nullptr};
    for (auto* button : dialog->findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("Release selected target")) release = button;
    }
    QVERIFY(release);
    const auto refresh = [&fixture, dialog] {
        emit fixture.service.changed();
        QCoreApplication::sendPostedEvents(dialog.data(), QEvent::MetaCall);
    };
    targets->setCurrentIndex(targets->findData(second));
    confirmed->setChecked(true);
    refresh();
    QCOMPARE(targets->currentData().toString(), second);
    QVERIFY(confirmed->isChecked());

    targets->setCurrentIndex(targets->findData(first));
    QVERIFY(!confirmed->isChecked());
    confirmed->setChecked(true);
    targets->setCurrentIndex(targets->findData(second));
    QVERIFY(!confirmed->isChecked());

    confirmed->setChecked(true);
    QVERIFY(guard.release(second, "execution-second"));
    QVERIFY(guard.reserve(second, "execution-replaced", "test-instance", error));
    refresh();
    QCOMPARE(targets->currentData().toString(), second);
    QVERIFY(!confirmed->isChecked());

    // The marker may change after the UI refresh but before the user clicks release.
    confirmed->setChecked(true);
    QVERIFY(guard.release(second, "execution-replaced"));
    QVERIFY(guard.reserve(second, "execution-latest", "test-instance", error));
    release->click();
    QVERIFY(!confirmed->isChecked());
    QCOMPARE(guard.markers().value(second).toObject().value("executionId").toString(),
             QStringLiteral("execution-latest"));
    QVERIFY(!fixture.service.acknowledgeTarget(second, QString{}));

    refresh();
    confirmed->setChecked(true);
    release->click();
    QVERIFY(!guard.markers().contains(second));
    QVERIFY(guard.markers().contains(first));
    refresh();
    QCOMPARE(targets->currentData().toString(), first);
    QVERIFY(!confirmed->isChecked());
    dialog->close();
    QTRY_VERIFY(dialog.isNull());
}

void McpTests::settingsDialogRefreshPreservesExpandedSessions()
{
    // 回归：refresh() 重建整棵树而根项默认折叠，勾选任意一条授权都会经
    // setGrant → changed → 排队的 refresh 把用户刚展开的层级收起来。
    // 两个会话才能证明恢复是按会话 ID 逐个生效的，而不是"全部展开"。
    eApp->init();
    Fixture f;
    f.makeSsh();
    TerminalCore secondCore{80, 24};
    LocalFake secondLocal;
    TerminalSession secondSession{&secondCore};
    secondSession.attach(&secondLocal, TerminalSession::Ownership::Borrowed,
                          TransportKind::LocalShell);
    static_cast<void>(secondSession.start());
    f.service.directory().add(&secondSession);
    QVERIFY(f.service.access().setEnabled(true));
    QWidget owner;
    QPointer<McpSettingsDialog> dialog = new McpSettingsDialog(&f.service, &owner);
    dialog->show();
    QTest::qWait(40);
    auto* tree = dialog->findChild<ElaTreeWidget*>();
    QVERIFY(tree && tree->topLevelItemCount() == 2);
    QVERIFY(!tree->topLevelItem(0)->isExpanded());
    tree->topLevelItem(0)->setExpanded(true);
    QVERIFY(tree->topLevelItem(0)->isExpanded());
    // 勾选"读取输出"：itemChanged → setGrant → changed → 排队 refresh。
    tree->topLevelItem(0)->setCheckState(2, Qt::Checked);
    QTRY_VERIFY(f.service.access().canRead(
        f.client, f.service.directory().entries().first()));
    QTRY_VERIFY(tree->topLevelItem(0)->isExpanded());
    QCOMPARE(tree->topLevelItem(0)->checkState(2), Qt::Checked);
    // 未展开的那一项不得被顺带展开。
    QVERIFY(!tree->topLevelItem(1)->isExpanded());
    // 直接触发 refresh 也要保留展开状态（会话状态变化走的就是这条路径）。
    tree->topLevelItem(1)->setExpanded(true);
    QVERIFY(tree->topLevelItem(1)->isExpanded());
    tree->setCurrentItem(tree->topLevelItem(0));
    const auto selectedSession = tree->topLevelItem(0)->data(0, Qt::UserRole).toString();
    emit f.service.changed();
    QTRY_VERIFY(tree->topLevelItem(0)->isExpanded() && tree->topLevelItem(1)->isExpanded());
    QTRY_VERIFY(tree->currentItem());
    QCOMPARE(tree->currentItem()->data(0, Qt::UserRole).toString(), selectedSession);
    // 会话从目录消失后不再保留其展开状态，重建出来的其余项也不受影响。
    f.service.directory().remove(&secondSession);
    emit f.service.changed();
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    QVERIFY(tree->topLevelItem(0)->isExpanded());
    dialog->close();
    QTRY_VERIFY(dialog.isNull());
}

void McpTests::accessStoreReportsDistinctSaveFailureReasons()
{
    // 锁被别的实例占用与状态目录未加固是两种失败：前者要用户关掉另一个实例，
    // 后者要修目录权限/属主。设置界面据此分别提示，因此原因必须能被区分。
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto state = root.path() + "/state";
    QVERIFY(QDir().mkpath(state));
    AccessStore store(state, {});
    QVERIFY(store.setEnabled(true));
    QVERIFY(store.lastSaveFailure() == AccessStore::SaveFailure::None);
    // 另一个实例持有同一把共享文件锁。
    QLockFile foreign(QDir(state).filePath("access.lock"));
    QVERIFY(foreign.tryLock(0));
    QVERIFY(!store.setEnabled(false));
    QCOMPARE(store.lastSaveFailure(),
             AccessStore::SaveFailure::Locked);
    // 原因被界面读走即清空，不能污染下一次成功保存。
    foreign.unlock();
    QVERIFY(store.setEnabled(false));
    QVERIFY(store.lastSaveFailure() == AccessStore::SaveFailure::None);
#ifndef Q_OS_WIN
    // 目录是符号链接时 secureDirectory() 必失败，属于"目录"这一类原因。
    const auto link = root.path() + "/link";
    QVERIFY(QFile::link(state, link));
    AccessStore linked(link, {});
    QVERIFY(!linked.setEnabled(true));
    QCOMPARE(linked.lastSaveFailure(),
             AccessStore::SaveFailure::Directory);
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

void McpTests::administratorOwnedObjectsMigrateToCurrentUser_data()
{
    QTest::addColumn<bool>("directory");
    QTest::newRow("endpoint-directory") << true;
    QTest::newRow("private-file") << false;
}

void McpTests::administratorOwnedObjectsMigrateToCurrentUser()
{
#ifdef Q_OS_WIN
    QFETCH(bool, directory);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto path = root.filePath(directory ? "instances" : "access.json");
    if (directory) {
        QVERIFY(QDir().mkpath(path));
    } else {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("fixture"), qint64(7));
    }
    BYTE adminSid[SECURITY_MAX_SID_SIZE];
    DWORD adminSize = sizeof(adminSid);
    QVERIFY(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminSid, &adminSize));
    auto native = QDir::toNativeSeparators(path).toStdWString();
    const auto setup = SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION, adminSid, nullptr, nullptr, nullptr);
    if (setup == ERROR_INVALID_OWNER || setup == ERROR_PRIVILEGE_NOT_HELD
        || setup == ERROR_ACCESS_DENIED) {
        QSKIP("Creating an Administrators-owned fixture requires an elevated test process.");
    }
    QCOMPARE(setup, DWORD(ERROR_SUCCESS));
    QVERIFY(directory ? secureDirectory(path) : secureFile(path));

    HANDLE token = nullptr;
    QVERIFY(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    const auto closeToken = qScopeGuard([&] { CloseHandle(token); });
    DWORD tokenSize = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &tokenSize);
    QByteArray userStorage(qsizetype(tokenSize), Qt::Uninitialized);
    QVERIFY(GetTokenInformation(token, TokenUser, userStorage.data(), tokenSize, &tokenSize));
    const auto* user = reinterpret_cast<const TOKEN_USER*>(userStorage.constData());
    PSID objectOwner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    QCOMPARE(GetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &objectOwner, nullptr, &dacl, nullptr, &descriptor), DWORD(ERROR_SUCCESS));
    const auto freeDescriptor = qScopeGuard([&] { LocalFree(descriptor); });
    QVERIFY2(EqualSid(objectOwner, user->User.Sid),
        "A legacy administrator-owned object must become owned by the current user.");
    QVERIFY(dacl);
    QCOMPARE(dacl->AceCount, WORD(1));
    void* entry = nullptr;
    QVERIFY(GetAce(dacl, 0, &entry));
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(entry);
    QCOMPARE(ace->Header.AceType, BYTE(ACCESS_ALLOWED_ACE_TYPE));
    QVERIFY(EqualSid(const_cast<DWORD*>(&ace->SidStart), user->User.Sid));
    QCOMPARE(ace->Mask, DWORD(FILE_ALL_ACCESS));
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    QVERIFY(GetSecurityDescriptorControl(descriptor, &control, &revision));
    QVERIFY(control & SE_DACL_PROTECTED);
    // Repeated startup must still be securable after migration.
    QVERIFY(directory ? secureDirectory(path) : secureFile(path));
#else
    QSKIP("Windows object ownership migration.");
#endif
}

void McpTests::unrelatedGroupOwnedDirectoryIsRejected()
{
#ifdef Q_OS_WIN
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto path = root.filePath("foreign");
    QVERIFY(QDir().mkpath(path));
    HANDLE token = nullptr;
    QVERIFY(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token));
    const auto closeToken = qScopeGuard([&] { CloseHandle(token); });
    TOKEN_PRIVILEGES enable{}, previous{};
    enable.PrivilegeCount = 1;
    QVERIFY(LookupPrivilegeValueW(nullptr, SE_RESTORE_NAME, &enable.Privileges[0].Luid));
    enable.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    DWORD previousSize = sizeof(previous);
    QVERIFY(AdjustTokenPrivileges(token, FALSE, &enable, sizeof(previous), &previous, &previousSize));
    const auto restorePrivileges = qScopeGuard([&] {
        AdjustTokenPrivileges(token, FALSE, &previous, 0, nullptr, nullptr);
    });
    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
        QSKIP("Creating a foreign group-owned fixture requires SeRestorePrivilege.");
    // Everyone belongs to the token but is not a valid owner identity. Accepting
    // every token group would silently take over this shared directory.
    BYTE everyoneSid[SECURITY_MAX_SID_SIZE];
    DWORD sidSize = sizeof(everyoneSid);
    QVERIFY(CreateWellKnownSid(WinWorldSid, nullptr, everyoneSid, &sidSize));
    auto native = QDir::toNativeSeparators(path).toStdWString();
    QCOMPARE(SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
        everyoneSid, nullptr, nullptr, nullptr), DWORD(ERROR_SUCCESS));
    // The production call must not rely on privileges enabled only for setup.
    QVERIFY(AdjustTokenPrivileges(token, FALSE, &previous, 0, nullptr, nullptr));
    QVERIFY(!secureDirectory(path));
#else
    QSKIP("Windows object ownership validation.");
#endif
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
            if (enabled && NovaTerm::Mcp::TestSupport::setGrant(service.access(), client, entry, true, {}, true)) {
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

int interopFixture(QCoreApplication& application, const QString& configurationFile, bool holdCommands = false)
{
    Fixture fixture;
    fixture.makeSsh();
    if (!fixture.enable(true)) return 1;
    if (!NovaTerm::Mcp::TestSupport::setTerminalGrant(fixture.service.access(), fixture.client,
            fixture.service.directory().entries().first(), true)) return 1;
    auto& targets = fixture.service.savedTargets();
    if (!targets.replace({{newId(), "Interop board", "ssh", "fixture.invalid:22", QByteArray(32, 'a')}})) return 1;
    const auto target = targets.localTargets().first();
    const auto identityVersion = fixture.service.access().identityVersion(fixture.client);
    if (!targets.setShared(fixture.client, identityVersion, target.id, target.configVersion, true)
        || !targets.setOpenAllowed(fixture.client, identityVersion, target.id, target.configVersion, true)) return 1;
    fixture.service.setTargetOpener([&](const SavedTargetSource&, const TargetConnections::Guard& allowed) {
        if (!allowed()) return TargetConnectionFacts{{}, {}, {}, "unavailable", "admission_invalidated", false};
        const auto entry = fixture.service.directory().entries().first();
        return TargetConnectionFacts{entry.id, entry.epoch, entry.attachmentId, "running", {}, false};
    });
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
        if (!holdCommands && SshTransportTestAccess::queued(*fixture.ssh) > 0) SshTransportTestAccess::finish(*fixture.ssh, true);
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

void McpTests::settingsDialogRoundTripsTerminalPermissions()
{
    eApp->init();
    Fixture f;
    QVERIFY(f.service.access().setEnabled(true));
    QWidget owner;
    QPointer<McpSettingsDialog> dialog = new McpSettingsDialog(&f.service, &owner);
    auto* tree = dialog->findChild<ElaTreeWidget*>();
    QVERIFY(tree && tree->topLevelItemCount() == 1);
    const auto entry = f.service.directory().entries().first();
    QVERIFY(!f.service.access().canRead(f.client, entry));
    tree->topLevelItem(0)->setCheckState(6, Qt::Checked);
    QTRY_VERIFY(f.service.access().canControlTerminal(f.client, entry));
    QVERIFY(f.service.access().canRead(f.client, entry));
    QVERIFY(!f.service.access().canRunCommand(f.client, entry));
    QVERIFY(!f.service.access().canRunScriptTask(f.client, entry));
    QVERIFY(!f.service.access().canUseElevation(f.client, entry));
    QCoreApplication::processEvents();
    tree->topLevelItem(0)->setCheckState(7, Qt::Checked);
    QTRY_VERIFY(f.service.access().canUseElevation(f.client, entry));
    QCoreApplication::processEvents();
    tree->topLevelItem(0)->setCheckState(6, Qt::Unchecked);
    QTRY_VERIFY(!f.service.access().canControlTerminal(f.client, entry));
    QVERIFY(!f.service.access().canUseElevation(f.client, entry));
    QVERIFY(f.service.access().canRead(f.client, entry));
    QCoreApplication::processEvents();
    tree->topLevelItem(0)->setCheckState(6, Qt::Checked);
    tree->topLevelItem(0)->setCheckState(7, Qt::Checked);
    QVERIFY(f.service.access().canUseElevation(f.client, entry));
    tree->topLevelItem(0)->setCheckState(2, Qt::Unchecked);
    QTRY_VERIFY(!f.service.access().canRead(f.client, entry));
    QVERIFY(!f.service.access().canControlTerminal(f.client, entry));
    QVERIFY(!f.service.access().canUseElevation(f.client, entry));
    QCoreApplication::processEvents();
    QCOMPARE(tree->topLevelItem(0)->checkState(6), Qt::Unchecked);
    QCOMPARE(tree->topLevelItem(0)->checkState(7), Qt::Unchecked);
}

void McpTests::settingsDialogRoundTripsClearPermission()
{
    using Model = NovaTerm::Ui::SessionPermissionModel;
    eApp->init();
    Fixture f;
    QVERIFY(f.service.access().setEnabled(true));
    QWidget owner;
    QPointer<McpSettingsDialog> dialog = new McpSettingsDialog(&f.service, &owner);
    auto* tree = dialog->findChild<ElaTreeWidget*>();
    QVERIFY(tree && tree->topLevelItemCount() == 1);
    const auto entry = f.service.directory().entries().first();
    const int clearColumn = Model::index(Model::Column::ClearTerminal);
    const int readColumn = Model::index(Model::Column::ReadOutput);
    const int inputColumn = Model::index(Model::Column::TerminalInput);
    const auto clickClear = [tree] {
        QStyleOptionViewItem option;
        option.rect = QRect(0, 0, 180, 30);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(70, 15),
            QPointF(70, 15), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        return tree->itemDelegateForColumn(clearColumn)->editorEvent(&release,
            tree->model(), option, tree->model()->index(0, clearColumn));
    };
    QVERIFY(!f.service.access().canClearTerminal(f.client, entry));
    QVERIFY(!clickClear());
    tree->topLevelItem(0)->setCheckState(inputColumn, Qt::Checked);
    QCoreApplication::processEvents();
    QVERIFY(!f.service.access().canClearTerminal(f.client, entry));
    QCOMPARE(tree->topLevelItem(0)->checkState(clearColumn), Qt::Unchecked);
    QVERIFY(clickClear());
    QVERIFY(f.service.access().canClearTerminal(f.client, entry));
    QCoreApplication::processEvents();
    QCOMPARE(tree->topLevelItem(0)->checkState(clearColumn), Qt::Checked);
    tree->topLevelItem(0)->setCheckState(inputColumn, Qt::Unchecked);
    QCoreApplication::processEvents();
    QVERIFY(f.service.access().canClearTerminal(f.client, entry));
    QVERIFY(clickClear());
    QVERIFY(!f.service.access().canClearTerminal(f.client, entry));
    QCoreApplication::processEvents();
    QVERIFY(clickClear());
    QVERIFY(f.service.access().canClearTerminal(f.client, entry));
    QCoreApplication::processEvents();
    tree->topLevelItem(0)->setCheckState(readColumn, Qt::Unchecked);
    QCoreApplication::processEvents();
    QVERIFY(!f.service.access().canClearTerminal(f.client, entry));
    QCOMPARE(tree->topLevelItem(0)->checkState(clearColumn), Qt::Unchecked);
}

void McpTests::settingsDialogRejectsElevationWithoutResettingActiveModel_data()
{
    QTest::addColumn<bool>("telnet");
    QTest::newRow("serial") << false;
    QTest::newRow("telnet") << true;
}

void McpTests::settingsDialogRejectsElevationWithoutResettingActiveModel()
{
    QFETCH(bool, telnet);
    eApp->init();
    Fixture f;
    // 使用假传输；MCP 状态变化后，旧 UI 项目的后端拒绝仍不得删除活动节点。
    f.session.close(CloseMode::Abort);
    QVERIFY(f.session.resetForReuse());
    f.session.attach(&f.local, TerminalSession::Ownership::Borrowed,
        telnet ? TransportKind::Telnet : TransportKind::Serial);
    QVERIFY(f.session.start());
    QVERIFY(f.enable());
    const auto entry = f.service.directory().entries().first();
    QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.service.access(), f.client, entry, true));
    QVERIFY(!f.service.access().canUseElevation(f.client, entry));
    QWidget owner;
    QPointer<McpSettingsDialog> dialog = new McpSettingsDialog(&f.service, &owner);
    auto* tree = dialog->findChild<ElaTreeWidget*>();
    QVERIFY(tree && tree->topLevelItemCount() == 1);
    const auto index = tree->indexFromItem(tree->topLevelItem(0), 7);
    const QPersistentModelIndex retained(index);
    QSignalSpy resets(tree->model(), &QAbstractItemModel::modelReset);
    QVERIFY(f.service.access().setEnabled(false));
    // 即使程序直接写入模型而绕过禁用的 UI，后端拒绝也不得同步删除活动节点。
    QVERIFY(tree->model()->setData(index, Qt::Checked, Qt::CheckStateRole));
    // Qt setData 尚在使用当前 item 时，权限拒绝不能同步 clear/rebuild 模型。
    QVERIFY(retained.isValid());
    QCOMPARE(resets.size(), 0);
    QVERIFY(!f.service.access().canUseElevation(f.client, entry));
    QTRY_VERIFY(!retained.isValid());
    QVERIFY(tree->topLevelItemCount() == 1);
    QCOMPARE(tree->topLevelItem(0)->checkState(7), Qt::Unchecked);
    QVERIFY(!f.service.access().canControlTerminal(f.client, entry));
}

void McpTests::settingsDialogElevationCheckboxRequiresSupportedContext_data()
{
    QTest::addColumn<QString>("connection");
    QTest::addColumn<bool>("control");
    QTest::addColumn<bool>("canGrant");
    QTest::newRow("serial") << QString("serial") << true << true;
    QTest::newRow("telnet") << QString("telnet") << true << true;
    QTest::newRow("local-without-input") << QString("local") << false << false;
    QTest::newRow("local-with-input") << QString("local") << true << true;
}

void McpTests::settingsDialogElevationCheckboxRequiresSupportedContext()
{
    QFETCH(QString, connection);
    QFETCH(bool, control);
    QFETCH(bool, canGrant);
    eApp->init();
    Fixture f;
    if (connection != "local") {
        f.session.close(CloseMode::Abort);
        QVERIFY(f.session.resetForReuse());
        f.session.attach(&f.local, TerminalSession::Ownership::Borrowed,
            connection == "serial" ? TransportKind::Serial : TransportKind::Telnet);
        QVERIFY(f.session.start());
    }
    QVERIFY(f.enable());
    QTRY_COMPARE(f.service.status(), QStringLiteral("Listening"));
    const auto entry = f.service.directory().entries().first();
    if (control) QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.service.access(), f.client, entry, true));
    QWidget owner;
    QPointer<McpSettingsDialog> dialog = new McpSettingsDialog(&f.service, &owner);
    auto* tree = dialog->findChild<ElaTreeWidget*>();
    QVERIFY(tree && tree->topLevelItemCount() == 1);
    const auto preview = qEnvironmentVariable("NOVATERM_MCP_PERMISSION_PREVIEW");
    if (!preview.isEmpty()) {
        const auto originalTheme = eTheme->getThemeMode();
        const auto restoreTheme = qScopeGuard([originalTheme] { eTheme->setThemeMode(originalTheme); });
        dialog->resize(1400, 860);
        dialog->show();
        for (const auto theme : {ElaThemeType::Light, ElaThemeType::Dark}) {
            eTheme->setThemeMode(theme);
            QTest::qWait(20);
            const auto path = preview + '-' + connection + (control ? "-input-" : "-no-input-")
                + (theme == ElaThemeType::Dark ? "dark.png" : "light.png");
            QVERIFY(dialog->grab().save(path));
        }
        dialog->hide();
    }
    const auto index = tree->indexFromItem(tree->topLevelItem(0), 7);
    const QPersistentModelIndex retained(index);
    QSignalSpy revoked(&f.service.access(), &AccessStore::revoked);
    QStyleOptionViewItem option;
    option.rect = QRect(0, 0, 120, 30);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(60, 15), QPointF(60, 15),
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    auto* delegate = tree->itemDelegateForColumn(7);
    QVERIFY(delegate);
    QCOMPARE(delegate->editorEvent(&release, tree->model(), option, index), canGrant);
    QCOMPARE(f.service.access().canUseElevation(f.client, entry), canGrant);
    if (!canGrant) {
        QVERIFY(!tree->topLevelItem(0)->text(7).isEmpty());
        QVERIFY(!tree->topLevelItem(0)->toolTip(7).isEmpty());
        QCOMPARE(revoked.size(), 0);
        QCOMPARE(tree->topLevelItem(0)->checkState(7), Qt::Unchecked);
        QCoreApplication::processEvents();
        QVERIFY(retained.isValid());
        QCOMPARE(f.service.access().canControlTerminal(f.client, entry), control);
    }
    if (connection == "local" && !control) {
        tree->topLevelItem(0)->setCheckState(6, Qt::Checked);
        QTRY_VERIFY(f.service.access().canControlTerminal(f.client, entry));
        QCoreApplication::processEvents();
        const auto enabledIndex = tree->indexFromItem(tree->topLevelItem(0), 7);
        QVERIFY(delegate->editorEvent(&release, tree->model(), option, enabledIndex));
        QVERIFY(f.service.access().canUseElevation(f.client, entry));
    }
}

void McpTests::terminalInteractionRequiresNewGrantAndDeduplicates()
{
    Fixture f;
    QVERIFY(f.enable(false, true));
    Host host;
    QVERIFY(host.start(f.runtime(), f.token()));
    auto read = f.identity(); read.insert("responseMode", "compact");
    auto initial = host.call("novaterm_read_context", read);
    QVERIFY(initial.value("ok").toBool());
    QVERIFY(initial.value("data").toObject().value("inputToken").toString().isEmpty());
    QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.service.access(), f.client, f.service.directory().entries().first(), true));
    initial = host.call("novaterm_read_context", read);
    const auto inputToken = initial.value("data").toObject().value("inputToken").toString();
    QVERIFY(!inputToken.isEmpty());
    auto input = f.identity();
    input.insert("inputToken", inputToken);
    input.insert("operationId", QUuid::createUuid().toString(QUuid::WithoutBraces));
    input.insert("waitMs", 0);
    input.insert("actions", QJsonArray{QJsonObject{{"type", "text"}, {"text", "python -i"}},
        QJsonObject{{"type", "key"}, {"key", "Enter"}}});
    QVERIFY(host.call("novaterm_interact_terminal", input).value("ok").toBool());
    QTRY_COMPARE(f.local.written, QByteArray("python -i\r"));
    QVERIFY(host.call("novaterm_interact_terminal", input).value("ok").toBool());
    QTest::qWait(20);
    QCOMPARE(f.local.written, QByteArray("python -i\r"));
    f.session.writeUserInput("x");
    input.insert("operationId", QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto stale = host.call("novaterm_interact_terminal", input);
    QVERIFY(!stale.value("ok").toBool());
    QCOMPARE(f.local.written, QByteArray("python -i\rx"));
}

void McpTests::commandTimeoutKeepsExecutionFacts()
{
    namespace C = NovaTerm::Capabilities;
    Fixture f;
    QVERIFY(f.makeScriptUpload());
    auto pending = std::make_shared<PendingExecutorState>();
    f.session.commandFacade()->installExecutor(std::make_unique<PendingCommandExecutor>(pending), f.session.statistics().generation);
    const auto entry = f.service.directory().entries().first();
    QVERIFY(f.enable(false, true));
    f.session.commandCoordinator()->notePromptReady(1, f.session.inputArbiter()->userInputGeneration());
    auto registry = C::CapabilityRegistry::terminalCapabilities();
    C::CapabilityAuthorization auth{[&f](const C::PrincipalId&, C::EntryKind, const C::ResourceBinding& resource,
        const C::Definition&) { return std::optional<C::AuthorizationSnapshot>{{resource, f.service.access().version(f.client)}}; }};
    C::CapabilityDispatcher dispatcher{registry, auth};
    C::McpCapabilityAdapter source{auth, C::PrincipalId{f.client}};
    C::SessionBinding binding{C::SessionId{entry.id}, C::AttachmentId{entry.attachmentId}, C::Epoch{entry.epoch}, entry.generation, entry.targetFingerprint};
    QJsonObject delivered;
    CommandSessionAdapter adapter{f.service.access(), f.service.directory(), f.root.path() + "/timeout-targets", "fixture",
        {[&delivered](const TaskId&, QJsonObject value) { delivered = std::move(value); },
            [](const TaskId&, std::chrono::milliseconds) {}, [](const QString&, const QJsonObject&) { return false; }, [] {}}, dispatcher};
    CommandCall call; call.task = TaskId{"timeout/request"}; call.connection = "timeout"; call.id = "request";
    call.client = ClientId{f.client};
    call.request = RunCommandRequest{NovaTerm::Mcp::SessionId{entry.id}, Epoch{entry.epoch}, "pwd", std::chrono::milliseconds(1)};
    adapter.start(std::move(call), entry, source.context(binding, f.service.access().version(f.client), C::OperationId{"timeout"},
        C::Clock::now() + std::chrono::seconds(10)));
    QVERIFY(!f.local.written.isEmpty());
    QVERIFY(delivered.isEmpty());
    QTest::qSleep(10); // Advance beyond the 1 ms budget without processing the backend timer.
    adapter.expire(TaskId{"timeout/request"});
    const auto error = delivered.value("error").toObject();
    QCOMPARE(error.value("code").toString(), QString("COMMAND_TIMEOUT"));
    const auto details = error.value("details").toObject();
    QVERIFY(!details.value("executionId").toString().isEmpty());
    QVERIFY(details.value("executionMayHaveStarted").toBool());
    QVERIFY(!details.value("terminationConfirmed").toBool());
    QVERIFY(!adapter.protectedTargets().isEmpty());
}

void McpTests::terminalInteractionSupportsEveryTransport_data()
{
    QTest::addColumn<int>("kind");
    QTest::newRow("local") << int(TransportKind::LocalShell);
    QTest::newRow("ssh") << int(TransportKind::Ssh);
    QTest::newRow("serial") << int(TransportKind::Serial);
    QTest::newRow("telnet") << int(TransportKind::Telnet);
    QTest::newRow("custom") << int(TransportKind::Custom);
}

void McpTests::terminalInteractionSupportsEveryTransport()
{
    QFETCH(int, kind);
    Fixture f;
    f.session.close(CloseMode::Abort);
    QVERIFY(f.session.resetForReuse());
    RuntimeConfig config;
    config.transportKind = static_cast<TransportKind>(kind);
    config.transport.insert("interactiveLineEnding", "crlf");
    f.session.attach(&f.local, TerminalSession::Ownership::Borrowed, config);
    QVERIFY(f.session.start());
    QVERIFY(f.enable());
    const auto entry = f.service.directory().entries().first();
    QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.service.access(), f.client, entry, true));
    Host host; QVERIFY(host.start(f.runtime(), f.token()));
    auto read = f.identity(); read.insert("responseMode", "compact");
    const auto observed = host.call("novaterm_read_context", read);
    QVERIFY(observed.value("ok").toBool());
    const auto inputToken = observed.value("data").toObject().value("inputToken").toString();
    QVERIFY2(!inputToken.isEmpty(), "A connected terminal must not be denied by its transport kind.");
    QVERIFY(f.local.written.isEmpty()); // No hidden Shell/OS probe.
    auto args = f.identity();
    args.insert("inputToken", inputToken);
    args.insert("operationId", QUuid::createUuid().toString(QUuid::WithoutBraces));
    args.insert("waitMs", 0);
    args.insert("actions", QJsonArray{QJsonObject{{"type", "text"}, {"text", "echo"}},
        QJsonObject{{"type", "paste"}, {"text", " hello"}},
        QJsonObject{{"type", "key"}, {"key", "c"}, {"modifiers", QJsonArray{"Ctrl"}}},
        QJsonObject{{"type", "line_ending"}}});
    const auto result = host.call("novaterm_interact_terminal", args);
    QVERIFY(result.value("ok").toBool());
    QTRY_COMPARE(f.local.written, QByteArray("echo hello\x03\r\n"));
    QCOMPARE(result.value("data").toObject().value("operation").toObject()
        .value("acceptedActionCount").toInt(-1), 4);
}

void McpTests::compactCapturesRetainNewestFour()
{
    Fixture f; QVERIFY(f.enable()); Host host; QVERIFY(host.start(f.runtime(), f.token()));
    emit f.local.readyRead(QByteArrayLiteral("capture-retention-marker\r\n"));
    QVERIFY(f.core.waitForIdle(1000));
    auto read = f.identity(); read.insert("responseMode", "compact");
    QStringList newest;
    for (int i = 0; i < 12; ++i) {
        const auto response = host.call("novaterm_read_context", read);
        QVERIFY(response.value("ok").toBool());
        newest.append(response.value("data").toObject().value("captureId").toString());
        if (newest.size() > 4) {
            auto search = f.identity(); search.insert("captureId", newest.takeFirst());
            search.insert("query", "capture-retention-marker");
            QVERIFY(!host.call("novaterm_search_context", search).value("ok").toBool());
        }
        for (const auto& id : newest) {
            auto search = f.identity(); search.insert("captureId", id);
            search.insert("query", "capture-retention-marker");
            const auto found = host.call("novaterm_search_context", search);
            QVERIFY2(found.value("ok").toBool(), "A newer capture was evicted before an older one.");
            QVERIFY(!found.value("data").toObject().value("matches").toArray().isEmpty());
        }
    }
}

void McpTests::compactBaseSnapshotsAreRateLimited()
{
    Fixture f; QVERIFY(f.enable()); Host host; QVERIFY(host.start(f.runtime(), f.token()));
    auto read = f.identity(); read.insert("responseMode", "compact");
    QList<qint64> captured;
    for (int i = 0; i < 14; ++i) {
        emit f.local.readyRead(QByteArray("snapshot-") + QByteArray::number(i) + "\r\n");
        QVERIFY(f.core.waitForIdle(1000));
        const auto response = host.call("novaterm_read_context", read);
        QVERIFY(response.value("ok").toBool());
        const auto stamp = QDateTime::fromString(response.value("data").toObject()
            .value("capturedAt").toString(), Qt::ISODateWithMs).toMSecsSinceEpoch();
        if (captured.isEmpty() || captured.last() != stamp) captured.append(stamp);
        QTest::qWait(30);
    }
    QVERIFY(captured.size() >= 2); // Must eventually publish new output, not freeze forever.
    for (int i = 1; i < captured.size(); ++i)
        QVERIFY2(captured[i] - captured[i - 1] >= 240,
            "Ordinary output must share one base snapshot for a 250 ms sampling interval.");
}

void McpTests::elevationAvailabilityUsesIdentityNotConnectionType()
{
    for (const auto kind : {TransportKind::Serial, TransportKind::Telnet, TransportKind::Custom}) {
        Fixture f;
        f.session.close(CloseMode::Abort); QVERIFY(f.session.resetForReuse());
        f.session.attach(&f.local, TerminalSession::Ownership::Borrowed, kind);
        QVERIFY(f.session.start()); QVERIFY(f.enable());
        const auto entry = f.service.directory().entries().first();
        QCOMPARE(f.service.access().elevationGrantDisabledReason(f.client, entry),
            QString("TERMINAL_PERMISSION_REQUIRED"));
        QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.service.access(), f.client, entry, true));
        QCOMPARE(f.service.access().elevationGrantDisabledReason(f.client, entry),
            QString{});
        QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.service.access(), f.client, entry, true, true));
        QVERIFY(f.service.access().canUseElevation(f.client, entry));
        QVERIFY(!f.service.access().hasElevationCredential(entry, "sudo"));
        QVERIFY(f.local.written.isEmpty());
    }
}

void McpTests::settingsDialogKeepsPendingGrantsForEveryTerminal_data()
{
    QTest::addColumn<int>("kind");
    QTest::addColumn<bool>("backendReady");
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    QTest::newRow("local-ready") << int(TransportKind::LocalShell) << true;
#endif
    QTest::newRow("local-backend-missing") << int(TransportKind::LocalShell) << false;
    QTest::newRow("ssh-ready") << int(TransportKind::Ssh) << true;
    QTest::newRow("ssh-backend-missing") << int(TransportKind::Ssh) << false;
    QTest::newRow("serial") << int(TransportKind::Serial) << false;
    QTest::newRow("telnet") << int(TransportKind::Telnet) << false;
    QTest::newRow("custom") << int(TransportKind::Custom) << false;
}

void McpTests::settingsDialogKeepsPendingGrantsForEveryTerminal()
{
    QFETCH(int, kind); QFETCH(bool, backendReady);
    eApp->init();
    Fixture f;
    if (static_cast<TransportKind>(kind) == TransportKind::Ssh) f.makeSsh();
    else if (static_cast<TransportKind>(kind) != TransportKind::LocalShell) {
        f.session.close(CloseMode::Abort); QVERIFY(f.session.resetForReuse());
        f.session.attach(&f.local, TerminalSession::Ownership::Borrowed, static_cast<TransportKind>(kind));
        QVERIFY(f.session.start());
    }
    if (!backendReady) f.session.commandFacade()->reset(f.session.statistics().generation);
    QVERIFY(f.enable());
    f.service.directory().setDisplayTitle(&f.session, QString::fromLatin1(QTest::currentDataTag()));
    const auto entry = f.service.directory().entries().first();
    QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.service.access(), f.client, entry, true));
    QWidget owner; McpSettingsDialog dialog(&f.service, &owner);
    auto* tree = dialog.findChild<ElaTreeWidget*>();
    QVERIFY(tree && tree->topLevelItemCount() == 1);
    QCOMPARE(tree->topLevelItem(0)->childCount(), 4);
    QStyleOptionViewItem option; option.rect = QRect(0, 0, 120, 30);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(60, 15), QPointF(60, 15),
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    const auto click = [&](int column) {
        return tree->itemDelegateForColumn(column)->editorEvent(&release, tree->model(), option,
            tree->indexFromItem(tree->topLevelItem(0), column));
    };
    QVERIFY(click(3));
    QTRY_COMPARE(f.service.access().commands(f.client, entry).size(), 4);
    QCoreApplication::processEvents();
    QCOMPARE(tree->topLevelItem(0)->checkState(3), Qt::Checked);
    QVERIFY(click(7));
    QTRY_VERIFY(f.service.access().canUseElevation(f.client, entry));
    QCoreApplication::processEvents();
    QCOMPARE(tree->topLevelItem(0)->checkState(7), Qt::Checked);
    if (!backendReady) QVERIFY(!tree->topLevelItem(0)->text(3).isEmpty());
    QVERIFY(!tree->topLevelItem(0)->text(7).isEmpty());
    QCOMPARE(tree->itemWidget(tree->topLevelItem(0),
        NovaTerm::Ui::SessionPermissionModel::index(
            NovaTerm::Ui::SessionPermissionModel::Column::Credentials))->isEnabled(), backendReady);
    const auto preview = qEnvironmentVariable("NOVATERM_MCP_GRANT_PREVIEW");
    if (!preview.isEmpty()) {
        const auto previousTheme = eTheme->getThemeMode();
        const auto restoreTheme = qScopeGuard([previousTheme] { eTheme->setThemeMode(previousTheme); });
        dialog.resize(1600, 900); dialog.show();
        for (const auto theme : {ElaThemeType::Light, ElaThemeType::Dark}) {
            eTheme->setThemeMode(theme); QTest::qWait(20);
            QVERIFY(dialog.grab().save(preview + '-' + QTest::currentDataTag()
                + (theme == ElaThemeType::Dark ? "-dark.png" : "-light.png")));
        }
        dialog.hide();
    }
    Host host; QVERIFY(host.start(f.runtime(), f.token()));
    const auto sessions = host.call("novaterm_list_sessions", {});
    const auto listed = sessions.value("data").toObject().value("sessions").toArray();
    QCOMPARE(listed.size(), 1);
    QVERIFY(listed[0].toObject().value("permissions").toObject().value("useElevation").toBool());
    QVERIFY(!listed[0].toObject().value("availability").toObject().value("elevationAvailable").toBool());
    QVERIFY(!listed[0].toObject().value("availability").toObject().value("elevationDisabledReason").toString().isEmpty());
    const auto commands = host.call("novaterm_list_commands", f.identity());
    QVERIFY(commands.value("ok").toBool());
    QCOMPARE(commands.value("data").toObject().value("executionEnabled").toBool(), backendReady);
    QCOMPARE(commands.value("data").toObject().value("commands").toArray().size(), backendReady ? 4 : 0);
    auto read = f.identity(); read.insert("responseMode", "compact");
    const auto observed = host.call("novaterm_read_context", read);
    auto elevate = f.identity(); elevate.insert("inputToken", observed.value("data").toObject().value("inputToken"));
    elevate.insert("operationId", QUuid::createUuid().toString(QUuid::WithoutBraces));
    elevate.insert("method", "sudo"); elevate.insert("mode", "shell");
    const auto denied = host.call("novaterm_request_elevation", elevate);
    QVERIFY(!denied.value("ok").toBool());
    if (!backendReady) QVERIFY(!f.service.access().setElevationCredential(entry, "sudo", "not-a-real-password"));
    QVERIFY(!f.service.access().hasElevationCredential(entry, "sudo"));
    QVERIFY(f.local.written.isEmpty());
    tree->topLevelItem(0)->setCheckState(2, Qt::Unchecked);
    QTRY_VERIFY(!f.service.access().canUseElevation(f.client, entry));
    QVERIFY(f.service.access().commands(f.client, entry).isEmpty());
}

void McpTests::settingsDialogRejectsRowFromPreviousClient()
{
    eApp->init();
    Fixture fixture;
    QVERIFY(fixture.enable());
    const auto entry = fixture.service.directory().entries().first();
    const auto otherClient = fixture.service.access().addClient(QStringLiteral("other"));
    QVERIFY(!otherClient.isEmpty());
    QWidget owner;
    McpSettingsDialog dialog(&fixture.service, &owner);
    auto* tree = dialog.findChild<ElaTreeWidget*>();
    QVERIFY(tree && tree->topLevelItemCount() == 1);
    ElaComboBox* clients = nullptr;
    for (auto* combo : dialog.findChildren<ElaComboBox*>()) {
        if (combo->findData(fixture.client) >= 0 && combo->findData(otherClient) >= 0) {
            clients = combo;
            break;
        }
    }
    QVERIFY(clients);
    clients->setCurrentIndex(clients->findData(fixture.client));
    auto* oldRow = tree->topLevelItem(0);
    QCOMPARE(oldRow->checkState(2), Qt::Checked);
    const auto firstVersion = fixture.service.access().version(fixture.client);
    const auto otherVersion = fixture.service.access().version(otherClient);
    const QPersistentModelIndex retained(tree->indexFromItem(oldRow, 2));
    {
        // 模拟客户端已切换、旧行的排队刷新尚未执行的窗口。
        const QSignalBlocker blocker(clients);
        clients->setCurrentIndex(clients->findData(otherClient));
    }
    oldRow->setCheckState(2, Qt::Unchecked);
    QVERIFY(retained.isValid());
    QCOMPARE(fixture.service.access().version(fixture.client), firstVersion);
    QCOMPARE(fixture.service.access().version(otherClient), otherVersion);
    QVERIFY(fixture.service.access().canRead(fixture.client, entry));
    QVERIFY(!fixture.service.access().canRead(otherClient, entry));
    QTRY_VERIFY(!retained.isValid());
    QCOMPARE(tree->topLevelItem(0)->checkState(2), Qt::Unchecked);
    QVERIFY(fixture.local.written.isEmpty());
}

void McpTests::settingsDialogRejectsStaleConnectionRow_data()
{
    QTest::addColumn<int>("kind");
    QTest::newRow("local") << int(TransportKind::LocalShell);
    QTest::newRow("ssh") << int(TransportKind::Ssh);
    QTest::newRow("serial") << int(TransportKind::Serial);
    QTest::newRow("telnet") << int(TransportKind::Telnet);
    QTest::newRow("custom") << int(TransportKind::Custom);
}

void McpTests::settingsDialogRejectsStaleConnectionRow()
{
    QFETCH(int, kind);
    eApp->init(); Fixture f;
    f.session.close(CloseMode::Abort); QVERIFY(f.session.resetForReuse());
    f.session.attach(&f.local, TerminalSession::Ownership::Borrowed, static_cast<TransportKind>(kind));
    QVERIFY(f.session.start()); QVERIFY(f.enable());
    const auto original = f.service.directory().entries().first();
    QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.service.access(), f.client, original, true, true));
    LocalFake replacement; QVERIFY(replacement.connectToHost());
    QWidget owner; McpSettingsDialog dialog(&f.service, &owner);
    auto* tree = dialog.findChild<ElaTreeWidget*>();
    QVERIFY(tree && tree->topLevelItemCount() == 1);
    auto* oldRow = tree->topLevelItem(0);
    QCOMPARE(oldRow->checkState(6), Qt::Checked);
    QCOMPARE(oldRow->checkState(7), Qt::Checked);
    const QPersistentModelIndex retained(tree->indexFromItem(oldRow, 7));
    f.session.attach(&replacement, TerminalSession::Ownership::Borrowed, static_cast<TransportKind>(kind));
    const auto current = f.service.directory().entries().first();
    QCOMPARE(current.id, original.id);
    QVERIFY(current.epoch != original.epoch);
    QVERIFY(!f.service.access().canControlTerminal(f.client, current));
    // Queued refresh has not run: a revoke on the previous connection must never
    // copy its still-checked input permission onto the replacement connection.
    oldRow->setCheckState(7, Qt::Unchecked);
    QVERIFY(retained.isValid()); // No deletion while itemChanged/setData is active.
    QVERIFY(!f.service.access().canControlTerminal(f.client, current));
    QVERIFY(!f.service.access().canUseElevation(f.client, current));
    QVERIFY(!f.service.access().canRead(f.client, current));
    QTRY_VERIFY(!retained.isValid());
    QVERIFY(replacement.written.isEmpty());
}

void McpTests::terminalInputReportsSequenceAndReleaseAdmission()
{
    Fixture f; QVERIFY(f.enable());
    QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.service.access(), f.client, f.service.directory().entries().first(), true));
    Host host; QVERIFY(host.start(f.runtime(), f.token()));
    auto read = f.identity(); read.insert("responseMode", "compact");
    auto input = f.identity(); input.insert("waitMs", 0);
    const auto observe = [&] { return host.call("novaterm_read_context", read).value("data").toObject(); };
    input.insert("inputToken", observe().value("inputToken"));
    input.insert("operationId", QUuid::createUuid().toString(QUuid::WithoutBraces));
    input.insert("actions", QJsonArray{QJsonObject{{"type", "text"}, {"text", "a"}}});
    QVERIFY(host.call("novaterm_interact_terminal", input).value("ok").toBool());
    QTRY_COMPARE(f.local.written, QByteArray("a"));
    input.insert("inputToken", observe().value("inputToken"));
    input.insert("operationId", QUuid::createUuid().toString(QUuid::WithoutBraces));
    QVERIFY(host.call("novaterm_interact_terminal", input).value("ok").toBool());
    QTRY_COMPARE(f.local.written, QByteArray("aa"));
    input.insert("operationId", QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto stale = host.call("novaterm_interact_terminal", input);
    QCOMPARE(stale.value("error").toObject().value("code").toString(), QString("INPUT_ALREADY_SUBMITTED"));
    QCOMPARE(f.local.written, QByteArray("aa"));
    input.insert("inputToken", observe().value("inputToken"));
    const auto releaseId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    input.insert("operationId", releaseId);
    input.insert("actions", QJsonArray{QJsonObject{{"type", "release"}}});
    const auto released = host.call("novaterm_interact_terminal", input);
    QVERIFY(released.value("ok").toBool());
    const auto operation = released.value("data").toObject().value("operation").toObject();
    QCOMPARE(operation.value("id").toString(), releaseId);
    QCOMPARE(operation.value("acceptedActionCount").toInt(-1), 1);
    QVERIFY(!operation.value("inputMayHaveBeenSent").toBool());
    QCOMPARE(f.local.written, QByteArray("aa"));
}

void McpTests::compactAttentionWaitCoalescesScrollAndExpires()
{
    Fixture f;
    QVERIFY(f.enable());
    Host host;
    QVERIFY(host.start(f.runtime(), f.token()));
    auto read = f.identity(); read.insert("responseMode", "compact");
    const auto first = host.call("novaterm_read_context", read);
    QVERIFY(first.value("ok").toBool());
    read.insert("sinceToken", first.value("data").toObject().value("nextToken"));
    read.insert("waitMs", 400); read.insert("waitFor", "attention");
    QElapsedTimer time; time.start();
    host.begin("novaterm_read_context", read);
    emit f.local.readyRead(QByteArray(10000, 'a') + "\r\n");
    const auto response = host.next(2000);
    QVERIFY(time.elapsed() >= 350);
    const auto result = response.value("result").toObject();
    QVERIFY(QJsonDocument(result).toJson(QJsonDocument::Compact).size() <= 8192);
    const auto data = result.value("structuredContent").toObject().value("data").toObject();
    QCOMPARE(data.value("wakeReason").toString(), QString("deadline"));
    QVERIFY(data.value("command").toObject().value("state") != "finished");
}

void McpTests::terminalCommandCompletionAndReadinessAreSeparateFromOutput()
{
    Fixture f;
    QVERIFY(f.makeScriptUpload());
    QVERIFY(f.enable());
    QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.service.access(), f.client, f.service.directory().entries().first(), true));
    Host host; QVERIFY(host.start(f.runtime(), f.token()));
    auto read = f.identity(); read.insert("responseMode", "compact");
    QVERIFY(host.call("novaterm_read_context", read).value("ok").toBool());
    emit f.local.readyRead(QByteArrayLiteral("\x1b]633;NT;PROMPT;1;0\a"));
    QTRY_VERIFY(f.session.terminalController()->readyForCommand());
    const auto observed = host.call("novaterm_read_context", read).value("data").toObject();
    auto args = f.identity(); args.insert("inputToken", observed.value("inputToken"));
    args.insert("operationId", QUuid::createUuid().toString(QUuid::WithoutBraces));
    args.insert("actions", QJsonArray{QJsonObject{{"type", "command"}, {"command", "pwd"}}});
    args.insert("waitMs", 1500);
    QTimer::singleShot(100, &f.session, [&f] {
        emit f.local.readyRead(QByteArrayLiteral("/tmp\r\n\x1b]633;NT;PROMPT;2;0\a"));
    });
    const auto result = host.call("novaterm_interact_terminal", args);
    QVERIFY2(result.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(result).toJson())));
    const auto data = result.value("data").toObject();
    QCOMPARE(data.value("wakeReason").toString(), QString("command_finished"));
    QCOMPARE(data.value("command").toObject().value("state").toString(), QString("finished"));
    QCOMPARE(data.value("command").toObject().value("exitCode").toInt(-1), 0);
    QVERIFY(data.value("inputReadiness").toObject().value("canSubmitCommand").toBool());
    QCOMPARE(f.local.written, QByteArray("pwd\r"));
}

void McpTests::compactCaptureExpansionDoesNotMoveLiveCursor()
{
    Fixture f; QVERIFY(f.enable()); Host host; QVERIFY(host.start(f.runtime(), f.token()));
    emit f.local.readyRead(QByteArrayLiteral("first-line\r\nsecond-line\r\n"));
    QVERIFY(f.core.waitForIdle(1000));
    auto read = f.identity(); read.insert("responseMode", "compact"); read.insert("maxLines", 1);
    auto result = host.call("novaterm_read_context", read);
    QVERIFY(result.value("ok").toBool());
    const auto data = result.value("data").toObject();
    const auto expand = data.value("expandToken").toString();
    QVERIFY(!expand.isEmpty());
    auto args = f.identity(); args.insert("responseMode", "compact");
    args.insert("expandToken", expand); args.insert("maxBytes", 3); args.insert("maxLines", 2);
    QByteArray text;
    for (int page = 0; page < 200; ++page) {
        const auto expanded = host.call("novaterm_read_context", args);
        QVERIFY(expanded.value("ok").toBool());
        const auto body = expanded.value("data").toObject();
        QVERIFY(!body.contains("inputToken"));
        for (const auto& line : body.value("viewport").toArray()) text += line.toObject().value("text").toString().toUtf8();
        if (body.value("expandToken").isNull()) break;
        QVERIFY(body.value("expandToken").toString() != args.value("expandToken").toString());
        args.insert("expandToken", body.value("expandToken"));
    }
    QVERIFY(text.contains("first-line")); QVERIFY(text.contains("second-line"));
    read.insert("sinceToken", data.value("nextToken")); read.remove("maxLines");
    const auto unchanged = host.call("novaterm_read_context", read).value("data").toObject();
    QVERIFY(!unchanged.value("outputChanged").toBool());
}

void McpTests::fullProjectionExpansionStartsAtOmittedLine()
{
    Fixture f; QVERIFY(f.enable()); Host host; QVERIFY(host.start(f.runtime(), f.token()));
    emit f.local.readyRead(QByteArrayLiteral("alpha\r\nbeta\r\ngamma\r\n"));
    QVERIFY(f.core.waitForIdle(1000));
    auto read = f.identity(); read.insert("responseMode", "full"); read.insert("maxLines", 1);
    const auto first = host.call("novaterm_read_context", read);
    QVERIFY(first.value("ok").toBool());
    const auto token = first.value("data").toObject().value("expandToken").toString();
    QVERIFY(!token.isEmpty());
    auto expand = f.identity(); expand.insert("responseMode", "full");
    expand.insert("expandToken", token); expand.insert("maxLines", 1);
    const auto second = host.call("novaterm_read_context", expand);
    QVERIFY(second.value("ok").toBool());
    QString text;
    for (const auto& line : second.value("data").toObject().value("viewport").toArray())
        text += line.toObject().value("text").toString();
    QVERIFY(text.contains(QStringLiteral("beta")));
    QVERIFY(!text.contains(QStringLiteral("alpha")));
}

void McpTests::compactExpansionAdvancesForEscapedLongLine()
{
    Fixture f; QVERIFY(f.enable()); Host host; QVERIFY(host.start(f.runtime(), f.token()));
    f.core.resize(8000, 2); QVERIFY(f.core.waitForIdle(1000));
    QByteArray text;
    for (int i = 0; i < 2500; ++i) text += QByteArrayLiteral("\\\"");
    emit f.local.readyRead(text + "\r\n"); QVERIFY(f.core.waitForIdle(1000));
    auto read = f.identity(); read.insert("responseMode", "compact"); read.insert("maxLines", 1);
    const auto first = host.call("novaterm_read_context", read).value("data").toObject();
    auto token = first.value("expandToken").toString(); QVERIFY(!token.isEmpty());
    auto expand = f.identity(); expand.insert("responseMode", "compact");
    QByteArray expanded;
    for (int page = 0; page < 100 && !token.isEmpty(); ++page) {
        expand.insert("expandToken", token);
        const auto response = host.call("novaterm_read_context", expand);
        QVERIFY(response.value("ok").toBool());
        QVERIFY(QJsonDocument(toolResult(response)).toJson(QJsonDocument::Compact).size() <= 8192);
        const auto result = response.value("data").toObject();
        const auto viewport = result.value("viewport").toArray();
        QVERIFY(!viewport.isEmpty());
        for (const auto& line : viewport)
            expanded += line.toObject().value("text").toString().toUtf8();
        const auto next = result.value("expandToken").toString();
        QVERIFY(next != token);
        token = next;
    }
    QVERIFY(token.isEmpty());
    QVERIFY(expanded.contains(text));
}

void McpTests::compactExpansionRejectsTooSmallUtf8Budget()
{
    Fixture f; QVERIFY(f.enable()); Host host; QVERIFY(host.start(f.runtime(), f.token()));
    emit f.local.readyRead(QString::fromUtf8(u8"中").toUtf8());
    QVERIFY(f.core.waitForIdle(1000));
    auto read = f.identity(); read.insert("responseMode", "compact"); read.insert("maxLines", 1);
    const auto first = host.call("novaterm_read_context", read).value("data").toObject();
    const auto token = first.value("expandToken").toString(); QVERIFY(!token.isEmpty());
    auto expand = f.identity(); expand.insert("responseMode", "compact");
    expand.insert("expandToken", token); expand.insert("maxBytes", 1); expand.insert("maxLines", 1);
    const auto response = host.call("novaterm_read_context", expand);
    QVERIFY(!response.value("ok").toBool());
    QCOMPARE(response.value("error").toObject().value("code").toString(),
        QStringLiteral("TEXT_BUDGET_TOO_SMALL"));
}

void McpTests::compactCursorSupportsFullProjection()
{
    Fixture f; QVERIFY(f.enable()); Host host; QVERIFY(host.start(f.runtime(), f.token()));
    emit f.local.readyRead(QByteArrayLiteral("full-snapshot-marker\r\n"));
    QVERIFY(f.core.waitForIdle(1000));
    auto read = f.identity(); read.insert("responseMode", "compact");
    const auto first = host.call("novaterm_read_context", read);
    QVERIFY(first.value("ok").toBool());
    read.insert("responseMode", "full");
    read.insert("includeViewport", true);
    read.insert("sinceToken", first.value("data").toObject().value("nextToken"));
    const auto full = host.call("novaterm_read_context", read);
    QVERIFY2(full.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(full).toJson())));
    const auto containsMarker = [](const QJsonObject& response) {
        const auto viewport = response.value("data").toObject().value("viewport").toArray();
        return std::any_of(viewport.begin(), viewport.end(), [](const QJsonValue& line) {
            return line.toObject().value("text").toString().contains("full-snapshot-marker");
        });
    };
    QVERIFY(containsMarker(full));
    read.remove("responseMode");
    read.insert("sinceToken", full.value("data").toObject().value("nextToken"));
    const auto implicitFull = host.call("novaterm_read_context", read);
    QVERIFY(implicitFull.value("ok").toBool());
    QVERIFY(containsMarker(implicitFull));

    const auto legacy = host.call("novaterm_read_context", f.identity());
    QVERIFY(legacy.value("ok").toBool());
    read.insert("responseMode", "full");
    read.insert("sinceToken", legacy.value("data").toObject().value("nextToken"));
    const auto migratedFull = host.call("novaterm_read_context", read);
    QVERIFY(migratedFull.value("ok").toBool());
    QVERIFY(containsMarker(migratedFull));
    read.insert("responseMode", "compact");
    read.insert("sinceToken", migratedFull.value("data").toObject().value("nextToken"));
    const auto explicitViewport = host.call("novaterm_read_context", read);
    QVERIFY(explicitViewport.value("ok").toBool());
    QVERIFY(containsMarker(explicitViewport));
    read.remove("includeViewport");
    const auto compact = host.call("novaterm_read_context", read);
    QVERIFY(compact.value("ok").toBool());
    QVERIFY(compact.value("data").toObject().value("viewport").toArray().isEmpty());
}

void McpTests::fullProjectionTruncationRequiresReset_data()
{
    QTest::addColumn<bool>("byteBudget");
    QTest::newRow("line-budget") << false;
    QTest::newRow("byte-budget") << true;
}

void McpTests::fullProjectionTruncationRequiresReset()
{
    QFETCH(bool, byteBudget);
    Fixture f; QVERIFY(f.enable()); Host host; QVERIFY(host.start(f.runtime(), f.token()));
    emit f.local.readyRead(QByteArrayLiteral("full-first\r\nfull-second\r\n"));
    QVERIFY(f.core.waitForIdle(1000));
    auto read = f.identity(); read.insert("responseMode", "full");
    read.insert("includeViewport", true);
    read.insert("maxBytes", byteBudget ? 3 : 65536);
    read.insert("maxLines", byteBudget ? 1024 : 1);
    const auto response = host.call("novaterm_read_context", read);
    QVERIFY(response.value("ok").toBool());
    const auto data = response.value("data").toObject();
    QVERIFY(!data.value("sourceTruncated").toBool());
    QVERIFY(data.value("outputTruncated").toBool());
    QVERIFY(data.value("resetRequired").toBool());
    QVERIFY(data.value("nextToken").isNull());
    const auto viewport = data.value("viewport").toArray();
    QVERIFY(!viewport.isEmpty());
    QCOMPARE(viewport.first().toObject().value("text").toString(),
        byteBudget ? QStringLiteral("ful") : QStringLiteral("full-first"));
    read.insert("responseMode", "compact");
    const auto compact = host.call("novaterm_read_context", read);
    QVERIFY(compact.value("ok").toBool());
    const auto compactData = compact.value("data").toObject();
    QVERIFY(compactData.value("projectionLimited").toBool());
    QVERIFY(!compactData.value("resetRequired").toBool());
    QVERIFY(!compactData.value("nextToken").toString().isEmpty());
    QVERIFY(!compactData.value("expandToken").toString().isEmpty());
}

void McpTests::compactExpansionPreservesSourceGap()
{
    Fixture f; QVERIFY(f.enable()); Host host; QVERIFY(host.start(f.runtime(), f.token()));
    QByteArray output;
    for (int line = 0; line < 1100; ++line) output += QByteArrayLiteral("x\r\n");
    emit f.local.readyRead(output); QVERIFY(f.core.waitForIdle(1000));
    auto read = f.identity(); read.insert("responseMode", "compact"); read.insert("maxLines", 1);
    const auto first = host.call("novaterm_read_context", read).value("data").toObject();
    QVERIFY(first.value("sourceGap").toBool());
    auto expand = f.identity(); expand.insert("responseMode", "compact");
    expand.insert("expandToken", first.value("expandToken")); expand.insert("maxLines", 1);
    const auto response = host.call("novaterm_read_context", expand);
    QVERIFY(response.value("ok").toBool());
    const auto data = response.value("data").toObject();
    QVERIFY(data.value("sourceGap").toBool());
    QVERIFY(data.value("sourceTruncated").toBool());
    QVERIFY(data.value("resetRequired").toBool());
}

void McpTests::maximumTerminalCommandIncludesConfiguredLineEnding()
{
    Fixture f; QVERIFY(f.makeScriptUpload()); QVERIFY(f.enable());
    QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.service.access(),
        f.client, f.service.directory().entries().first(), true));
    Host host; QVERIFY(host.start(f.runtime(), f.token()));
    emit f.local.readyRead(QByteArrayLiteral("\x1b]633;NT;PROMPT;1;0\a"));
    QTRY_VERIFY(f.session.terminalController()->readyForCommand());
    auto read = f.identity(); read.insert("responseMode", "compact");
    const auto observed = host.call("novaterm_read_context", read).value("data").toObject();
    const QString command(16 * 1024, QLatin1Char('a'));
    auto args = f.identity(); args.insert("inputToken", observed.value("inputToken"));
    args.insert("operationId", QUuid::createUuid().toString(QUuid::WithoutBraces));
    args.insert("actions", QJsonArray{QJsonObject{{"type", "command"}, {"command", command}}});
    args.insert("waitMs", 0);
    const auto response = host.call("novaterm_interact_terminal", args);
    QVERIFY2(response.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(response).toJson())));
    QTRY_COMPARE(f.local.written, command.toUtf8() + '\r');
}

void McpTests::managedElevationRejectsWithoutAdapter()
{
    Fixture f; QVERIFY(f.enable());
    const auto entry = f.service.directory().entries().first();
    QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.service.access(), f.client, entry, true, true));
    Host host; QVERIFY(host.start(f.runtime(), f.token()));
    auto read = f.identity(); read.insert("responseMode", "compact");
    const auto data = host.call("novaterm_read_context", read).value("data").toObject();
    auto args = f.identity(); args.insert("inputToken", data.value("inputToken"));
    args.insert("operationId", QUuid::createUuid().toString(QUuid::WithoutBraces));
    args.insert("method", "su"); args.insert("mode", "shell");
    const auto result = host.call("novaterm_request_elevation", args);
    QVERIFY(!result.value("ok").toBool());
    QVERIFY(f.local.written.isEmpty());
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
    if (arguments.size() == 3 && arguments[1] == "--shutdown-fixture")
        return interopFixture(application, arguments[2], true);
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
void McpTests::clearTerminalIsExplicitIdempotentAndInvalidatesObservation_data()
{
    QTest::addColumn<QString>("version"); QTest::addColumn<bool>("serial"); QTest::addColumn<bool>("all");
    QTest::newRow("2025-ssh-all") << QString(ProtocolVersion) << false << true;
    QTest::newRow("2025-uart-history") << QString(ProtocolVersion) << true << false;
    QTest::newRow("2026-ssh-history") << QString("2026-07-28") << false << false;
    QTest::newRow("2026-uart-all") << QString("2026-07-28") << true << true;
}
void McpTests::workflowWaitAndTasksUseBothProtocols_data()
{
    QTest::addColumn<QString>("version"); QTest::addColumn<int>("kind");
    for (const auto& version : {QString(ProtocolVersion), QString("2026-07-28")})
        for (const auto kind : {TransportKind::Ssh, TransportKind::Serial})
            QTest::newRow(qPrintable(version + (kind == TransportKind::Ssh ? "-ssh" : "-uart")))
                << version << int(kind);
}
void McpTests::workflowWaitCancellationAndMcpReenable_data()
{
    QTest::addColumn<QString>("version");
    QTest::newRow("2025") << QString(ProtocolVersion);
    QTest::newRow("2026") << QString("2026-07-28");
}
void McpTests::workflowWaitCancellationAndMcpReenable()
{
    QFETCH(QString, version);
    Fixture f; QVERIFY(f.enable());
    auto entry = f.service.directory().entries().first();
    QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.service.access(), f.client, entry, true));
    Host host; QVERIFY(host.start(f.runtime(), f.token(), {}, false, version));
    auto wait = f.identity(); wait.insert("source", "log"); wait.insert("texts", QJsonArray{"NEVER_MATCH"});
    wait.insert("waitMs", 30000);
    for (int i = 0; i < 20; ++i) {
        const auto cancelled = f.service.statistics().value("cancelled").toString().toULongLong();
        const auto admitted = f.service.statistics().value("requests").toString().toULongLong();
        const int id = host.begin("novaterm_wait_text", wait);
        // RPC 已进入后端后取消；下一次同客户端等待不能被不可达续扫记录堵住。
        QTRY_COMPARE(f.service.statistics().value("requests").toString().toULongLong(), admitted + 1);
        if (version == "2026-07-28") host.cancelPendingRequests();
        else host.send({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"},
            {"params", QJsonObject{{"requestId", id}}}});
        QTRY_COMPARE(f.service.statistics().value("cancelled").toString().toULongLong(), cancelled + 1);
    }
    wait.insert("waitMs", 0);
    const auto after = host.call("novaterm_wait_text", wait);
    QVERIFY2(after.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(after).toJson())));
    QCOMPARE(after.value("data").toObject().value("status").toString(), "timeout");
    QVERIFY(f.local.written.isEmpty());
    QVERIFY(f.service.access().setEnabled(false));
    QVERIFY(f.enable());
    entry = f.service.directory().entries().first();
    QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.service.access(), f.client, entry, true));
    Host enabled; QVERIFY(enabled.start(f.runtime(), f.token(), {}, false, version));
    auto read = f.identity(); read.insert("responseMode", "compact");
    auto start = f.identity(); start.insert("operationId", newId());
    start.insert("inputToken", enabled.call("novaterm_read_context", read).value("data").toObject().value("inputToken"));
    start.insert("actions", QJsonArray{QJsonObject{{"type", "text"}, {"text", "after-enable"}}});
    const auto admitted = enabled.call("novaterm_start_task", start);
    QVERIFY2(admitted.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(admitted).toJson())));
    QTRY_COMPARE(f.local.written, QByteArray("after-enable"));
}
void McpTests::workflowWaitAndTasksUseBothProtocols()
{
    QFETCH(QString, version); QFETCH(int, kind);
    Fixture f;
    f.session.close(CloseMode::Abort); QVERIFY(f.session.resetForReuse());
    RuntimeConfig config; config.transportKind = static_cast<TransportKind>(kind);
    config.transport.insert("interactiveLineEnding", "crlf");
    f.session.attach(&f.local, TerminalSession::Ownership::Borrowed, config);
    QVERIFY(f.session.start()); QVERIFY(f.enable());
    const auto entry = f.service.directory().entries().first();
    QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.service.access(), f.client, entry, true));
    f.core.resize(80, 2); QVERIFY(f.core.waitForIdle());
    QVERIFY(f.core.writeInput("PASS_OLD\r\nold\r\nend\r\n").fullyAccepted()); QVERIFY(f.core.waitForIdle());
    Host host; QVERIFY(host.start(f.runtime(), f.token(), {}, false, version));
    const auto directory = host.call("novaterm_list_sessions").value("data").toObject().value("sessions").toArray();
    const auto taskCapabilities = directory.first().toObject().value("availability").toObject().value("terminalTasks").toObject();
    QVERIFY(taskCapabilities.value("canStart").toBool()); QVERIFY(taskCapabilities.value("canObserve").toBool());
    auto wait = f.identity(); wait.insert("source", "log"); wait.insert("texts", QJsonArray{"PASS"});
    const auto old = host.call("novaterm_wait_text", wait);
    QVERIFY2(old.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(old).toJson())));
    QCOMPARE(old.value("data").toObject().value("status").toString(), "timeout");
    wait.insert("sinceToken", old.value("data").toObject().value("nextToken"));
    QVERIFY(f.core.writeInput("PASS_NEW\r\nnext\r\nend\r\n").fullyAccepted()); QVERIFY(f.core.waitForIdle());
    const auto matched = host.call("novaterm_wait_text", wait);
    QCOMPARE(matched.value("data").toObject().value("status").toString(), "matched");
    QVERIFY(!matched.value("data").toObject().value("provesSuccess").toBool());
    QVERIFY(f.local.written.isEmpty());
    auto screen = f.identity(); screen.insert("source", "screen"); screen.insert("start", "current");
    screen.insert("texts", QJsonArray{"login:"});
    QVERIFY(f.core.writeInput("\rlogin: ").fullyAccepted()); QVERIFY(f.core.waitForIdle());
    screen.insert("waitMs", 1000);
    const auto prompt = host.call("novaterm_wait_text", screen);
    QCOMPARE(prompt.value("data").toObject().value("status").toString(), "matched");
    QVERIFY(!prompt.value("data").toObject().value("revision").toString().isEmpty());
    auto observe = f.identity(); observe.insert("responseMode", "compact");
    const auto context = host.call("novaterm_read_context", observe).value("data").toObject();
    auto start = f.identity(); start.insert("inputToken", context.value("inputToken"));
    start.insert("operationId", newId()); start.insert("timeoutMs", 600000);
    start.insert("actions", QJsonArray{QJsonObject{{"type", "text"}, {"text", "long-job"}},
        QJsonObject{{"type", "line_ending"}}});
    const auto admitted = host.call("novaterm_start_task", start);
    QVERIFY2(admitted.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(admitted).toJson())));
    const auto task = admitted.value("data").toObject().value("taskId").toString(); QVERIFY(!task.isEmpty());
    QTRY_COMPARE(f.local.written, QByteArray("long-job\r\n"));
    // 另一个协议连接恢复受理结果，查询和精确启动重试都不能再次输入。
    Host retry; QVERIFY(retry.start(f.runtime(), f.token(), {}, false, version));
    auto query = f.identity(); query.insert("operationId", start.value("operationId"));
    const auto recovered = retry.call("novaterm_query_task", query);
    QCOMPARE(recovered.value("data").toObject().value("taskId").toString(), task);
    const auto replay = retry.call("novaterm_start_task", start);
    QCOMPARE(replay.value("data").toObject().value("taskId").toString(), task);
    QCOMPARE(f.local.written, QByteArray("long-job\r\n"));
    auto conflict = start; conflict.insert("timeoutMs", 600001);
    QCOMPARE(retry.call("novaterm_start_task", conflict).value("error").toObject().value("code").toString(), "INPUT_SEQUENCE_CONFLICT");
    query.remove("operationId"); query.insert("taskId", task);
    query.insert("sinceToken", admitted.value("data").toObject().value("baseline").toObject().value("nextToken"));
    const auto running = retry.call("novaterm_query_task", query).value("data").toObject();
    QCOMPARE(running.value("status").toString(), "running");
    QVERIFY(!running.value("completed").toBool()); QVERIFY(running.value("exitCode").isNull());
    QVERIFY(!running.value("terminationConfirmed").toBool());
    QVERIFY(running.value("output").toObject().value("source") == "log");
    // 人工接管后旧任务停止不能抢回人的控制，也不能把新命令当成旧任务。
    f.session.writeUserInput("human-command\r");
    const auto bytes = f.local.written;
    auto stop = f.identity(); stop.insert("taskId", task); stop.insert("operationId", newId());
    const auto stopped = retry.call("novaterm_stop_task", stop);
    QCOMPARE(stopped.value("error").toObject().value("code").toString(), "CONTROL_INVALID");
    QCOMPARE(f.local.written, bytes);
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(f.service.access(), f.client, entry, false));
    QVERIFY(!retry.call("novaterm_query_task", query).value("ok").toBool());
    QCOMPARE(f.local.written, bytes);
}
void McpTests::clearTerminalIsExplicitIdempotentAndInvalidatesObservation()
{
    QFETCH(QString, version); QFETCH(bool, serial); QFETCH(bool, all);
    Fixture f;
    if (serial) {
        f.session.close(CloseMode::Abort); QVERIFY(f.session.resetForReuse());
        f.session.attach(&f.local, TerminalSession::Ownership::Borrowed, TransportKind::Serial);
        QVERIFY(f.session.start());
    } else f.makeSsh();
    QVERIFY(f.enable());
    f.core.resize(80, 2); QVERIFY(f.core.waitForIdle());
    QByteArray before;
    for (int i=0; i<80; ++i) before += "OLD_HISTORY_" + QByteArray::number(i) + "\r\n";
    QVERIFY(f.core.writeInput(before).fullyAccepted()); QVERIFY(f.core.waitForIdle());
    Host deniedHost; QVERIFY(deniedHost.start(f.runtime(), f.token(), {}, false, version));
    auto logArgs = f.identity(); logArgs.insert("source", "log"); logArgs.insert("start", "tail");
    auto baseline = deniedHost.call("novaterm_read_context", logArgs).value("data").toObject();
    auto args = f.identity(); args.insert("operationId", QUuid::createUuid().toString(QUuid::WithoutBraces));
    args.insert("scope", all ? "all" : "scrollback"); args.insert("expectedGeneration", baseline.value("cacheGeneration"));
    args.insert("clearToken", baseline.value("clearToken"));
    const auto generation = f.core.cacheGeneration();
    QVERIFY(!deniedHost.call("novaterm_clear_terminal", args).value("ok").toBool());
    QCOMPARE(f.core.cacheGeneration(), generation);
    const auto entry = f.service.directory().entries().first();
    QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.service.access(), f.client, entry, true));
    QVERIFY(NovaTerm::Mcp::TestSupport::applyOne(f.service.access(), f.client, entry,
        PermissionKind::ClearTerminal, std::nullopt, true));
    Host host; QVERIFY(host.start(f.runtime(), f.token(), {}, false, version));
    auto legacyArgs = f.identity();
    const auto legacy = host.call("novaterm_read_context", legacyArgs).value("data").toObject();
    auto compactArgs = f.identity(); compactArgs.insert("responseMode", "compact");
    const auto compact = host.call("novaterm_read_context", compactArgs).value("data").toObject();
    QVERIFY(!compact.value("inputToken").toString().isEmpty());
    auto smallBudget = f.identity(); smallBudget.insert("responseMode", "full"); smallBudget.insert("maxBytes", 24); smallBudget.insert("maxLines", 1);
    const auto expanded = host.call("novaterm_read_context", smallBudget).value("data").toObject();
    QVERIFY(!expanded.value("expandToken").toString().isEmpty());
    baseline = host.call("novaterm_read_context", logArgs).value("data").toObject();
    args.insert("expectedGeneration", baseline.value("cacheGeneration")); args.insert("clearToken", baseline.value("clearToken"));
    const auto cleared = host.call("novaterm_clear_terminal", args);
    QVERIFY2(cleared.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(cleared).toJson())));
    const auto data = cleared.value("data").toObject();
    QCOMPARE(data.value("status").toString(), QString("applied"));
    QVERIFY(data.value("cacheGeneration").toString().toULongLong() > generation);
    QCOMPARE(f.core.scrollbackLineCount(), 0);
    QCOMPARE(data.value("baseline").toObject().value("fragments").toArray().size(), 0);
    for (const auto& old : {legacy, compact}) {
        auto continuation = f.identity(); continuation.insert("sinceToken", old.value("nextToken"));
        if (old == compact) continuation.insert("responseMode", "compact");
        QVERIFY(!host.call("novaterm_read_context", continuation).value("ok").toBool());
        auto search = f.identity(); search.insert("captureId", old.value("captureId")); search.insert("query", "OLD");
        QVERIFY(!host.call("novaterm_search_context", search).value("ok").toBool());
    }
    auto expand = f.identity(); expand.insert("expandToken", expanded.value("expandToken"));
    QCOMPARE(host.call("novaterm_read_context", expand).value("error").toObject().value("code").toString(),
        QString("CAPTURE_NOT_AVAILABLE"));
    auto input = f.identity(); input.insert("inputToken", compact.value("inputToken"));
    input.insert("operationId", QUuid::createUuid().toString(QUuid::WithoutBraces));
    input.insert("actions", QJsonArray{QJsonObject{{"type", "key"}, {"key", "Tab"}}});
    input.insert("waitMs", 0);
    QVERIFY(!host.call("novaterm_interact_terminal", input).value("ok").toBool());
    auto oldLog = f.identity(); oldLog.insert("source", "log"); oldLog.insert("sinceToken", baseline.value("nextToken"));
    QCOMPARE(host.call("novaterm_read_context", oldLog).value("data").toObject().value("status").toString(), QString("invalidated"));
    QVERIFY(f.core.writeInput("NEW_AFTER_CLEAR\r\nNEW_AFTER_CLEAR\r\nNEW_AFTER_CLEAR\r\n").fullyAccepted()); QVERIFY(f.core.waitForIdle());
    const auto afterGeneration = f.core.cacheGeneration(); const auto lines = f.core.scrollbackLineCount(); QVERIFY(lines > 0);
    Host retry; QVERIFY(retry.start(f.runtime(), f.token(), {}, false, version));
    QCOMPARE(retry.call("novaterm_clear_terminal", args), cleared);
    QCOMPARE(f.core.cacheGeneration(), afterGeneration); QCOMPARE(f.core.scrollbackLineCount(), lines);
    auto conflict = args; conflict.insert("scope", all ? "scrollback" : "all");
    QCOMPARE(retry.call("novaterm_clear_terminal", conflict).value("error").toObject().value("code").toString(), QString("INPUT_SEQUENCE_CONFLICT"));
    const auto fresh = retry.call("novaterm_read_context", compactArgs);
    QVERIFY(fresh.value("ok").toBool());
    QVERIFY(!QJsonDocument(fresh).toJson().contains("OLD_HISTORY_0"));
    QVERIFY(NovaTerm::Mcp::TestSupport::applyOne(f.service.access(), f.client, entry,
        PermissionKind::ClearTerminal, std::nullopt, false));
    QVERIFY(!retry.call("novaterm_clear_terminal", args).value("ok").toBool());
    QCOMPARE(f.core.scrollbackLineCount(), lines);
    QVERIFY(f.local.written.isEmpty());
}
void McpTests::localCacheMutationsInvalidateRetainedObservations_data()
{
    QTest::addColumn<int>("mutation");
    QTest::newRow("menu-clear-all") << 0;
    QTest::newRow("menu-clear-history") << 1;
    QTest::newRow("device-erase-history") << 2;
    QTest::newRow("resize-pop-history") << 3;
}
void McpTests::localCacheMutationsInvalidateRetainedObservations()
{
    QFETCH(int, mutation);
    Fixture f; QVERIFY(f.enable());
    f.core.resize(80, 2); QVERIFY(f.core.waitForIdle());
    QVERIFY(f.core.writeInput("BEFORE_CLEAR\r\nBEFORE_CLEAR\r\nBEFORE_CLEAR\r\nBEFORE_CLEAR\r\n").fullyAccepted());
    QVERIFY(f.core.waitForIdle());
    Host host; QVERIFY(host.start(f.runtime(), f.token()));
    auto legacyArgs = f.identity();
    auto compactArgs = f.identity(); compactArgs.insert("responseMode", "compact");
    const auto legacy = host.call("novaterm_read_context", legacyArgs).value("data").toObject();
    const auto compact = host.call("novaterm_read_context", compactArgs).value("data").toObject();
    QVERIFY(!legacy.value("nextToken").toString().isEmpty()); QVERIFY(!compact.value("nextToken").toString().isEmpty());
    auto boundedArgs = f.identity(); boundedArgs.insert("responseMode", "full");
    boundedArgs.insert("maxBytes", 24); boundedArgs.insert("maxLines", 1);
    const auto bounded = host.call("novaterm_read_context", boundedArgs).value("data").toObject();
    QVERIFY(!bounded.value("expandToken").toString().isEmpty());
    const auto generation = f.core.cacheGeneration();
    if (mutation == 0) QVERIFY(f.core.clearAll());
    else if (mutation == 1) QVERIFY(f.core.clearScrollback());
    else if (mutation == 2) QVERIFY(f.core.writeInput("\x1b[3J").fullyAccepted());
    else f.core.resize(80, 12);
    QVERIFY(f.core.waitForIdle()); // no GUI events are needed to publish the generation
    QVERIFY(f.core.cacheGeneration() != generation);
    auto expandArgs = f.identity(); expandArgs.insert("expandToken", bounded.value("expandToken"));
    QCOMPARE(host.call("novaterm_read_context", expandArgs).value("error").toObject().value("code").toString(),
        QString("CAPTURE_NOT_AVAILABLE"));
    for (const auto& capture : {legacy, compact}) {
        auto continuation = f.identity(); continuation.insert("sinceToken", capture.value("nextToken"));
        if (capture == compact) continuation.insert("responseMode", "compact");
        const auto response = host.call("novaterm_read_context", continuation);
        QCOMPARE(response.value("error").toObject().value("code").toString(), QString("CURSOR_STALE"));
        auto search = f.identity(); search.insert("captureId", capture.value("captureId")); search.insert("query", "BEFORE");
        QCOMPARE(host.call("novaterm_search_context", search).value("error").toObject().value("code").toString(), QString("CAPTURE_NOT_AVAILABLE"));
    }
    const auto fresh = host.call("novaterm_read_context", compactArgs);
    QVERIFY(fresh.value("ok").toBool());
    if (mutation == 0) QVERIFY(!QJsonDocument(fresh).toJson().contains("BEFORE_CLEAR"));
    QVERIFY(f.local.written.isEmpty());
}
void McpTests::clearAdapterTracksSessionObjectReuseAndDestruction()
{
    Fixture f;
    f.session.close(CloseMode::Abort);
    class FlowControlledLocal final : public ITransport {
    public:
        bool connectToHost() override { online = true; emit connected(); return true; }
        void disconnect() override { online = false; emit disconnected(); }
        void write(const QByteArray&) override {}
        void resizeTerminal(int, int) override {}
        bool isConnected() const override { return online; }
        QString errorString() const override { return {}; }
        bool setReadPaused(bool) override { return true; }
        bool online{false};
    } flow;
    auto session = std::make_unique<TerminalSession>(&f.core);
    session->attach(&flow, TerminalSession::Ownership::Borrowed, TransportKind::LocalShell);
    QVERIFY(session->start()); f.service.directory().add(session.get());
    QVERIFY(f.service.access().setEnabled(true));
    const auto authorize = [&] {
        const auto entry = f.service.directory().find(session->id().toString(QUuid::WithoutBraces));
        return entry && NovaTerm::Mcp::TestSupport::setGrant(f.service.access(), f.client, *entry, true)
            && NovaTerm::Mcp::TestSupport::applyOne(f.service.access(), f.client, *entry,
                PermissionKind::ClearTerminal, std::nullopt, true);
    };
    const auto key = randomBytes();
    TerminalClearAdapter adapter(f.service.access(), f.service.directory(), "test", key, nullptr);
    const auto request = [&] {
        const auto entry = f.service.directory().find(session->id().toString(QUuid::WithoutBraces));
        ReadContextRequest read; read.session = NovaTerm::Mcp::SessionId{entry->id}; read.epoch = Epoch{entry->epoch};
        read.source = ContextSource::Log; read.start = LogInitialPosition::Tail;
        const auto baseline = readLogContext(f.service.access(), f.service.directory(), "test", f.client, key, read);
        if (isError(baseline)) return ClearRequest{};
        const auto data = std::get<QJsonObject>(baseline);
        return ClearRequest{read.session, read.epoch, OperationId{newId()}, true,
            data.value("cacheGeneration").toString().toULongLong(), data.value("clearToken").toString()};
    };
    QVERIFY(authorize()); QVERIFY(f.core.waitForIdle());
    auto first = request(); QVERIFY(!first.operation.isEmpty());
    std::optional<QJsonObject> result;
    adapter.call(f.client, "first", first, [&result](auto response) { result = response; });
    QTRY_VERIFY(result.has_value()); QVERIFY(result->value("ok").toBool());
    session->close(CloseMode::Abort); QVERIFY(session->resetForReuse());
    session->attach(&flow, TerminalSession::Ownership::Borrowed, TransportKind::LocalShell);
    QVERIFY(session->start()); QVERIFY(authorize()); QVERIFY(f.core.waitForIdle());
    auto second = request(); QVERIFY(!second.operation.isEmpty()); result.reset();
    const auto beforeEntry = f.service.directory().find(second.session.str()); QVERIFY(beforeEntry);
    QVERIFY(f.service.access().canClearTerminal(f.client, *beforeEntry));
    QVERIFY(f.core.writeInput(QByteArray(6 * 1024 * 1024, 'q')).fullyAccepted());
    adapter.call(f.client, "second", second, [&result](auto response) { result = response; });
    QVERIFY2(!result.has_value(), result ? qPrintable(QString::fromUtf8(QJsonDocument(*result).toJson())) : "");
    session->inputArbiter()->submitUserInput("human");
    QVERIFY(f.core.waitForIdle(30000));
    QTRY_VERIFY(result.has_value());
    const auto afterEntry = f.service.directory().find(second.session.str());
    QVERIFY(afterEntry); QCOMPARE(afterEntry->epoch, second.epoch.str());
    QVERIFY(f.service.access().canClearTerminal(f.client, *afterEntry));
    QCOMPARE(result->value("error").toObject().value("code").toString(), QString("CANCELLED"));
    auto third = request(); QVERIFY(!third.operation.isEmpty()); result.reset();
    QVERIFY(f.core.writeInput(QByteArray(6 * 1024 * 1024, 'q')).fullyAccepted());
    adapter.call(f.client, "third", third, [&result](auto response) { result = response; });
    session.reset();
    QVERIFY(result.has_value());
    QCOMPARE(result->value("error").toObject().value("code").toString(), QString("SESSION_NOT_AVAILABLE"));
    QVERIFY(f.core.waitForIdle(30000));
}
void McpTests::clearTicketRejectsHumanTakeoverBeforeAdmission()
{
    Fixture f; QVERIFY(f.enable());
    const auto entry = f.service.directory().entries().first();
    QVERIFY(NovaTerm::Mcp::TestSupport::applyOne(f.service.access(), f.client, entry,
        PermissionKind::ClearTerminal, std::nullopt, true));
    const auto key = randomBytes();
    TerminalClearAdapter adapter(f.service.access(), f.service.directory(), "test", key, nullptr);
    QVERIFY(f.core.waitForIdle());
    ReadContextRequest read; read.session = NovaTerm::Mcp::SessionId{entry.id}; read.epoch = Epoch{entry.epoch};
    read.source = ContextSource::Log; read.start = LogInitialPosition::Tail;
    const auto baseline = readLogContext(f.service.access(), f.service.directory(), "test", f.client, key, read);
    QVERIFY(!isError(baseline)); const auto data = std::get<QJsonObject>(baseline);
    ClearRequest request{read.session, read.epoch, OperationId{newId()}, true,
        data.value("cacheGeneration").toString().toULongLong(), data.value("clearToken").toString()};
    f.session.writeUserInput("human");
    const auto generation = f.core.cacheGeneration();
    std::optional<QJsonObject> result;
    adapter.call(f.client, "clear", request, [&result](auto response) { result = response; });
    QTRY_VERIFY(result.has_value());
    QCOMPARE(result->value("error").toObject().value("code").toString(), QString("CONTROL_INVALID"));
    QCOMPARE(f.core.cacheGeneration(), generation);
    QCOMPARE(f.local.written, QByteArray("human"));
}
void McpTests::expandDistinguishesEvictionFromCacheInvalidation()
{
    Fixture f; QVERIFY(f.enable());
    f.core.resize(80, 2); QVERIFY(f.core.waitForIdle());
    QVERIFY(f.core.writeInput("one\r\ntwo\r\nthree\r\nfour\r\n").fullyAccepted());
    QVERIFY(f.core.waitForIdle());
    Host host; QVERIFY(host.start(f.runtime(), f.token()));
    auto read = f.identity(); read.insert("responseMode", "full"); read.insert("maxLines", 1);
    const auto first = host.call("novaterm_read_context", read).value("data").toObject();
    const auto token = first.value("expandToken").toString(); QVERIFY(!token.isEmpty());
    // Four later captures evict this bounded projection without changing the
    // cache generation. Its existing CAPTURE_EXPIRED behavior must stay.
    for (int i = 0; i < 4; ++i) QVERIFY(host.call("novaterm_read_context", read).value("ok").toBool());
    auto expand = f.identity(); expand.insert("expandToken", token);
    QCOMPARE(host.call("novaterm_read_context", expand).value("error").toObject().value("code").toString(),
        QString("CAPTURE_EXPIRED"));
    const auto generation = f.core.cacheGeneration();
    QVERIFY(f.core.clearScrollback()); QVERIFY(f.core.waitForIdle());
    QVERIFY(f.core.cacheGeneration() != generation);
    // The signed old generation still identifies invalidation even when the
    // projection itself has already been evicted.
    QCOMPARE(host.call("novaterm_read_context", expand).value("error").toObject().value("code").toString(),
        QString("CAPTURE_NOT_AVAILABLE"));
    QVERIFY(f.local.written.isEmpty());
}
#include "McpTests.moc"
