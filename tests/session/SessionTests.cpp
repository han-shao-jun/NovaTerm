#include "core/terminal/TerminalCore.h"
#include "credential/CredentialStore.h"
#include "profile/ProfileStore.h"
#include "session/InteractiveStreamFramer.h"
#include "session/SessionCommandCoordinator.h"
#include "session/ShellIntegration.h"
#include "session/LocalShellProfile.h"
#include "session/LocalSessionScriptProvider.h"
#include "session/SessionInputArbiter.h"
#include "session/SessionStore.h"
#include "session/TerminalSession.h"
#include "session/SessionInputPump.h"
#include "transport/ITransport.h"

#include <QSignalSpy>
#include <QKeyEvent>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

class FakeTransport final : public ITransport
{
    Q_OBJECT
public:
    explicit FakeTransport(bool supportsReconnect = true, QObject* parent = nullptr)
        : ITransport(parent)
        , _supportsReconnect(supportsReconnect)
    {
    }

    bool connectToHost() override
    {
        ++connectAttempts;
        if (failedAttempts > 0) {
            --failedAttempts;
            return false;
        }
        if (_connected)
            return true;
        _connected = true;
        QMetaObject::invokeMethod(this, [this] { emit connected(); },
                                  Qt::QueuedConnection);
        return true;
    }
    void disconnect() override
    {
        if (!_connected)
            return;
        _connected = false;
        emit disconnected();
    }
    void write(const QByteArray& data) override
    {
        writes.append(data);
        emit bytesWritten(data.size());
    }
    void resizeTerminal(int columns, int rows) override
    {
        size = QSize(columns, rows);
    }
    bool isConnected() const override { return _connected; }
    bool setReadPaused(bool paused) override
    {
        readPaused = paused;
        return true;
    }
    TransportCapabilities capabilities() const override
    {
        auto result = TransportCapability::PauseReads
            | TransportCapability::ResizeTerminal;
        if (_supportsReconnect)
            result |= TransportCapability::Reconnect;
        return result;
    }
    QString errorString() const override { return {}; }

    void simulateRemoteDisconnect()
    {
        if (!_connected)
            return;
        _connected = false;
        emit disconnected();
    }

    /// 按 ITransport 约定的顺序发出结构化错误 + 文本错误。
    void simulateStructuredError(TransportErrorCategory category,
                                const QString& message, bool retryable)
    {
        emit transportError(TransportError{category, 42, message, retryable});
        emit errorOccurred(message);
    }

    /// 只发文本错误（模拟未实现 transportError 的后端）。
    void simulatePlainError(const QString& message)
    {
        emit errorOccurred(message);
    }

    QByteArray writes;
    QSize size;
    bool readPaused{false};
    int connectAttempts{0};
    int failedAttempts{0};

private:
    bool _connected{false};
    bool _supportsReconnect{true};
};

class SessionTests final : public QObject
{
    Q_OBJECT
private slots:
    void serialAutomaticReconnect();
    void automaticReconnectDisabledForZeroAndOtherProtocols();
    void lifecycleReachesRunningThenClosed();
    void manualDisconnectKeepsTransportReconnectable_data();
    void manualDisconnectKeepsTransportReconnectable();
    void enterReconnectsBySessionType();
    void customSessionWithoutCapabilityDoesNotReconnect();
    void runtimeConfigIsSnapshot();
    void restoreMetadataRoundTrip();
    void persistentStoresRejectSecrets();
    void reconnectBumpsGenerationAndKeepsHandlersLive();
    void structuredTransportErrorSetsSessionCategory();
    void agentContextFiltersProgressWrapDuplicatesAndAlternate();
    void inputPumpOffsetsPreservePendingSuffix();
    void userInputPreemptsPartialMcpWrite();
    void staleAndDuplicateMcpLeasesAreRejected();
    void emptyInputKeepsMcpLease();
    void interactiveMarkersNeverReachTerminalCore();
    void forgedAndOrdinaryOscRemainVisible();
    void framingBufferIsBoundedAndResetDropsOldPartialMarker();
    void interactiveCommandRequiresReadyPrompt();
    void userTypingInvalidatesPreviouslyReadyPrompt();
    void interactiveCommandCapturesCombinedFrameInOrder();
    void userInputCancelsStartedInteractiveCommand();
    void sshWithoutTrustedPromptRunsBoundedCommand();
    void unverifiedLongEchoHidesSuffixAcrossCarriageReturn();
    void unverifiedLongEchoHidesSuffixAcrossBackspaceRedraw();
    void unverifiedLongEchoHidesPartialSuffixBeforeLineRedraw();
    void cancellingUnverifiedCommandPreservesPartialEcho();
    void unverifiedCommandDeadlineReportsTimeout();
    void unknownInteractiveProfileDoesNotEnableCommands();
    void configuredDevicePromptDoesNotProbeTransport();
    void configuredDevicePromptRequiresSilence();
    void terminalSessionUsesConfiguredPromptProfile();
    void localShellPresetsCarryExplicitIntegration();
    void sshShellEnvironmentRequiresExplicitProfile();
    void localScriptProviderWritesExactlyRequestedPath();
    void shellPromptMarkerCompletesCurrentCommand();
    void clinkHookEnablesCmdOnlyWhenInstalled();
    void unconfiguredFramerLeavesTerminalBytesUntouched();
    void transportRebindDropsPreviousPromptProfile();
    void devicePasswordAndSplitAlternateScreenAreNotReady();
    void userInputStopsUnsentLongCommandTail();
    void agentContextJoinsHistorySeamAndBoundsUtf8();
    void secretServiceCredentialStoreSurvivesRestart();
};

void SessionTests::localScriptProviderWritesExactlyRequestedPath()
{
    QTemporaryDir root;
    QVERIFY2(root.isValid(), qPrintable(root.errorString()));
    const QString target = root.filePath(QStringLiteral("scripts/setup.sh"));
    QVERIFY(QDir().mkpath(QFileInfo(target).absolutePath()));
    LocalSessionScriptProvider provider;
    QSignalSpy finished(&provider, &ISessionScriptProvider::finished);
    ScriptWriteRequest request;
    request.requestId = 31;
    request.content = QByteArrayLiteral("#!/bin/sh\nprintf exact-body\n");
    request.targetPath = target;
    request.workingDirectory = root.path();

    QVERIFY(!QFile::exists(target));
    QVERIFY(provider.writeScript(request));
    QTRY_COMPARE(finished.count(), 1);
    const auto result = qvariant_cast<ScriptWriteResult>(finished.takeFirst().front());
    QCOMPARE(result.requestId, quint64(31));
    QVERIFY(result.success);
    QFile file(target);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), request.content);
}

void SessionTests::userInputPreemptsPartialMcpWrite()
{
    SessionInputArbiter arbiter;
    FakeTransport transport;
    QVERIFY(transport.connectToHost());
    arbiter.bind(&transport, 7);
    QVERIFY(arbiter.acquireMcpLease(41, 7));
    QVERIFY(arbiter.submitMcpInput(41, QByteArrayLiteral("echo par")));

    QSignalSpy preempted(&arbiter, &SessionInputArbiter::mcpPreempted);
    arbiter.submitUserInput(QByteArrayLiteral("x"));

    QCOMPARE(preempted.count(), 1);
    QCOMPARE(preempted.first().at(0).toULongLong(), quint64(41));
    QCOMPARE(preempted.first().at(1).toBool(), true);
    QCOMPARE(transport.writes, QByteArrayLiteral("echo parx"));
    QVERIFY(!arbiter.submitMcpInput(41, QByteArrayLiteral("tial\r")));
}

void SessionTests::staleAndDuplicateMcpLeasesAreRejected()
{
    SessionInputArbiter arbiter;
    FakeTransport transport;
    QVERIFY(transport.connectToHost());
    arbiter.bind(&transport, 9);

    QVERIFY(!arbiter.acquireMcpLease(10, 8));
    QVERIFY(!arbiter.acquireMcpLease(0, 9));
    QVERIFY(arbiter.acquireMcpLease(10, 9));
    QVERIFY(!arbiter.acquireMcpLease(11, 9));
    QVERIFY(!arbiter.submitMcpInput(11, QByteArrayLiteral("wrong")));
    QCOMPARE(transport.writes, QByteArray{});

    arbiter.releaseMcpLease(11);
    QVERIFY(arbiter.hasMcpLease());
    arbiter.releaseMcpLease(10);
    QVERIFY(!arbiter.hasMcpLease());
}

void SessionTests::emptyInputKeepsMcpLease()
{
    SessionInputArbiter arbiter;
    FakeTransport transport;
    QVERIFY(transport.connectToHost());
    arbiter.bind(&transport, 3);
    QVERIFY(arbiter.acquireMcpLease(22, 3));

    QSignalSpy preempted(&arbiter, &SessionInputArbiter::mcpPreempted);
    QVERIFY(arbiter.submitMcpInput(22, {}));
    arbiter.submitUserInput({});

    QVERIFY(arbiter.hasMcpLease());
    QCOMPARE(preempted.count(), 0);
    QCOMPARE(transport.writes, QByteArray{});
}

void SessionTests::interactiveMarkersNeverReachTerminalCore()
{
    InteractiveStreamFramer framer;
    InteractiveCommandProfile profile;
    profile.shellIntegration = true;
    framer.configure(profile);
    framer.reset(9, QByteArrayLiteral("nonce-1"));
    const auto first = framer.consume(
        QByteArrayLiteral("out\x1b]633;NT;END;non"));
    const auto second = framer.consume(
        QByteArrayLiteral("ce-1;0\x07prompt$ "));

    QCOMPARE(first.visibleBytes, QByteArrayLiteral("out"));
    QVERIFY(first.events.isEmpty());
    QCOMPARE(second.visibleBytes, QByteArrayLiteral("prompt$ "));
    QCOMPARE(second.events.size(), 1);
    QCOMPARE(second.events.front().kind,
             InteractiveStreamEventKind::CommandFinished);
    QCOMPARE(second.events.front().exitCode, std::optional<int>{0});
}

void SessionTests::forgedAndOrdinaryOscRemainVisible()
{
    InteractiveStreamFramer framer;
    InteractiveCommandProfile profile;
    profile.shellIntegration = true;
    framer.configure(profile);
    framer.reset(4, QByteArrayLiteral("current"));
    const QByteArray forged =
        QByteArrayLiteral("\x1b]633;NT;END;old;0\x07");
    const QByteArray ordinary = QByteArrayLiteral("\x1b]633;A\x07");

    const auto result = framer.consume(forged + ordinary);

    QCOMPARE(result.visibleBytes, forged + ordinary);
    QVERIFY(result.events.isEmpty());
}

void SessionTests::framingBufferIsBoundedAndResetDropsOldPartialMarker()
{
    InteractiveCommandProfile profile;
    profile.shellIntegration = true;
    profile.maxMarkerBytes = 32;
    InteractiveStreamFramer framer;
    framer.configure(profile);
    framer.reset(2, QByteArrayLiteral("nonce"));

    const QByteArray oversized = QByteArrayLiteral("\x1b]633;NT;END;")
        + QByteArray(40, 'x');
    const auto overflow = framer.consume(oversized);
    QCOMPARE(overflow.visibleBytes, oversized);
    QCOMPARE(overflow.events.size(), 1);
    QCOMPARE(overflow.events.front().kind,
             InteractiveStreamEventKind::FramingError);

    const auto partial = framer.consume(QByteArrayLiteral("\x1b]633;NT;END;non"));
    QVERIFY(partial.visibleBytes.isEmpty());
    framer.reset(3, QByteArrayLiteral("new"));
    const auto afterReset = framer.consume(QByteArrayLiteral("plain"));
    QCOMPARE(afterReset.visibleBytes, QByteArrayLiteral("plain"));
    QVERIFY(afterReset.events.isEmpty());
}

void SessionTests::interactiveCommandRequiresReadyPrompt()
{
    FakeTransport transport;
    QVERIFY(transport.connectToHost());
    SessionInputArbiter arbiter;
    arbiter.bind(&transport, 5);
    InteractiveStreamFramer framer;
    framer.reset(5);
    SessionCommandCoordinator coordinator(&arbiter, &framer);
    coordinator.reset(5);

    CommandExecutionRequest request;
    request.requestId = 71;
    request.command = QByteArrayLiteral("uname -srm");
    request.executionNonce = QByteArrayLiteral("exec-71");
    request.expectedPromptGeneration = 3;

    QVERIFY(!coordinator.submit(request));
    QCOMPARE(transport.writes, QByteArray{});
}

void SessionTests::userTypingInvalidatesPreviouslyReadyPrompt()
{
    FakeTransport transport;
    QVERIFY(transport.connectToHost());
    SessionInputArbiter arbiter;
    arbiter.bind(&transport, 12);
    InteractiveStreamFramer framer;
    InteractiveCommandProfile profile;
    profile.shellIntegration = true;
    framer.configure(profile);
    framer.reset(12);
    SessionCommandCoordinator coordinator(&arbiter, &framer);
    coordinator.reset(12);
    coordinator.handleInteractiveEvent({
        InteractiveStreamEventKind::PromptReady, 1, 0});
    QVERIFY(coordinator.isPromptReady());

    arbiter.submitUserInput(QByteArrayLiteral("user typed ahead"));

    QVERIFY(!coordinator.isPromptReady());
    coordinator.handleInteractiveEvent({
        InteractiveStreamEventKind::PromptReady, 2, 0});
    QVERIFY(coordinator.isPromptReady());
}

void SessionTests::interactiveCommandCapturesCombinedFrameInOrder()
{
    TerminalCore core(80, 24);
    FakeTransport transport;
    QVERIFY(transport.connectToHost());
    SessionInputArbiter arbiter;
    arbiter.bind(&transport, 6);
    InteractiveStreamFramer framer;
    InteractiveCommandProfile profile;
    profile.shellIntegration = true;
    framer.configure(profile);
    framer.reset(6);
    SessionCommandCoordinator coordinator(&arbiter, &framer);
    coordinator.reset(6);
    SessionInputPump pump(&transport, &core, &framer);
    connect(&pump, &SessionInputPump::interactiveEvent,
            &coordinator, &SessionCommandCoordinator::handleInteractiveEvent);
    connect(&pump, &SessionInputPump::interactiveBytes,
            &coordinator, &SessionCommandCoordinator::handleInteractiveBytes);
    pump.start();

    coordinator.handleInteractiveEvent({
        InteractiveStreamEventKind::PromptReady, 4, std::nullopt});
    CommandExecutionRequest request;
    request.requestId = 72;
    request.command = QByteArrayLiteral("uname -srm");
    request.executionNonce = QByteArrayLiteral("exec-72");
    request.expectedPromptGeneration = 4;
    QSignalSpy finished(&coordinator, &SessionCommandCoordinator::finished);
    QVERIFY(coordinator.submit(request));
    QCOMPARE(transport.writes, QByteArrayLiteral("uname -srm\r"));

    emit transport.readyRead(
        QByteArrayLiteral("uname -srm\r\n\x1b]633;NT;START;exec-72\x07")
        + QByteArrayLiteral("Linux armv7l\r\n")
        + QByteArrayLiteral("\x1b]633;NT;END;exec-72;0\x07root# "));

    QTRY_COMPARE(finished.count(), 1);
    const auto result = qvariant_cast<CommandExecutionResult>(
        finished.first().front());
    QCOMPARE(result.outcome, CommandExecutionOutcome::Completed);
    QCOMPARE(result.standardOutput, QByteArrayLiteral("Linux armv7l\r\n"));
    QCOMPARE(result.exitCode, std::optional<int>{0});
    QVERIFY(result.executionMayHaveStarted);
    QVERIFY(result.terminationConfirmed);
    QVERIFY(!coordinator.isPromptReady());
    QVERIFY(core.waitForIdle());
    const auto state = core.terminalState();
    QVERIFY(std::any_of(state.viewport.begin(), state.viewport.end(),
                        [](const auto& line) {
        return line.text.find("Linux armv7l") != std::string::npos;
    }));
}

void SessionTests::userInputCancelsStartedInteractiveCommand()
{
    FakeTransport transport;
    QVERIFY(transport.connectToHost());
    SessionInputArbiter arbiter;
    arbiter.bind(&transport, 8);
    InteractiveStreamFramer framer;
    InteractiveCommandProfile profile;
    profile.shellIntegration = true;
    framer.configure(profile);
    framer.reset(8);
    SessionCommandCoordinator coordinator(&arbiter, &framer);
    coordinator.reset(8);
    coordinator.handleInteractiveEvent({
        InteractiveStreamEventKind::PromptReady, 2, std::nullopt});

    CommandExecutionRequest request;
    request.requestId = 73;
    request.command = QByteArrayLiteral("sleep 5");
    request.executionNonce = QByteArrayLiteral("exec-73");
    request.expectedPromptGeneration = 2;
    QSignalSpy finished(&coordinator, &SessionCommandCoordinator::finished);
    QVERIFY(coordinator.submit(request));

    arbiter.submitUserInput(QByteArrayLiteral("x"));

    QCOMPARE(finished.count(), 1);
    const auto result = qvariant_cast<CommandExecutionResult>(
        finished.first().front());
    QCOMPARE(result.outcome, CommandExecutionOutcome::Cancelled);
    QVERIFY(result.executionMayHaveStarted);
    QVERIFY(!result.terminationConfirmed);
    QCOMPARE(transport.writes, QByteArrayLiteral("sleep 5\rx"));
}

void SessionTests::sshWithoutTrustedPromptRunsBoundedCommand()
{
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Ssh;
    const auto profile = ShellIntegration::profileFor(runtime);
    QVERIFY(profile);
    QVERIFY(profile->allowUnverifiedPrompt);

    TerminalCore core(80, 24);
    FakeTransport transport;
    QVERIFY(transport.connectToHost());
    SessionInputArbiter arbiter;
    arbiter.bind(&transport, 30);
    InteractiveStreamFramer framer;
    framer.configure(*profile);
    framer.reset(30);
    SessionCommandCoordinator coordinator(&arbiter, &framer);
    coordinator.configure(*profile);
    coordinator.reset(30);
    SessionInputPump pump(&transport, &core, &framer);
    connect(&pump, &SessionInputPump::interactiveEvent,
            &coordinator, &SessionCommandCoordinator::handleInteractiveEvent);
    connect(&pump, &SessionInputPump::interactiveBytes,
            &coordinator, &SessionCommandCoordinator::handleInteractiveBytes);
    pump.start();

    CommandExecutionRequest request;
    request.requestId = 301;
    request.command = QByteArrayLiteral("pwd");
    request.executionNonce = QByteArrayLiteral("blind-301");
    request.expectedPromptGeneration = 0;
    QSignalSpy finished(&coordinator, &SessionCommandCoordinator::finished);
    QVERIFY(coordinator.submit(request));
    const QByteArray submitted = transport.writes;
    QVERIFY(submitted.startsWith(QByteArrayLiteral("pwd;")));
    QVERIFY(submitted.endsWith('\r'));
    const qsizetype split = submitted.indexOf(QByteArrayLiteral(";__nvterm")) + 4;
    QVERIFY(split > 4 && split < submitted.size());
    emit transport.readyRead(QByteArrayLiteral("root# ") + submitted.left(split));
    emit transport.readyRead(submitted.mid(split)
        + QByteArrayLiteral("\n/root\n\x1b]633;NT;END;blind-301;0\x07"));

    QTRY_COMPARE(finished.count(), 1);
    const auto result = qvariant_cast<CommandExecutionResult>(finished.first().front());
    QCOMPARE(result.outcome, CommandExecutionOutcome::Completed);
    QCOMPARE(result.exitCode, std::optional<int>{0});
    QVERIFY(result.terminationConfirmed);
    QVERIFY(result.standardOutput.contains(QByteArrayLiteral("/root")));
    QVERIFY(core.waitForIdle());
    const auto state = core.terminalState();
    QByteArray visible;
    for (const auto& line : state.viewport)
        visible += QByteArray::fromStdString(line.text);
    QVERIFY(visible.contains(QByteArrayLiteral("root# pwd")));
    QVERIFY(visible.contains(QByteArrayLiteral("/root")));
    QVERIFY(!visible.contains(QByteArrayLiteral("__nvterm")));
    QVERIFY(!visible.contains(QByteArrayLiteral("NT;END")));
}

void SessionTests::unverifiedLongEchoHidesSuffixAcrossCarriageReturn()
{
    InteractiveCommandProfile profile;
    profile.shellIntegration = true;
    profile.requiresStartMarker = false;
    profile.allowUnverifiedPrompt = true;
    InteractiveStreamFramer framer;
    framer.configure(profile);
    framer.reset(32);
    const QByteArray nonce = QByteArrayLiteral("blind-long-echo");
    const QByteArray suffix = QByteArrayLiteral(
        ";__nvterm_rc=$?;printf '\\033]633;NT;END;blind-long-echo;%d\\007' \"$__nvterm_rc\"");
    framer.beginTransaction(nonce, suffix);

    const QByteArray echo = QByteArrayLiteral("root# cat /etc/os-release")
        + suffix.left(45) + QByteArrayLiteral("\r") + suffix.mid(45)
        + QByteArrayLiteral("\r\nNAME=Buildroot\r\n")
        + QByteArrayLiteral("\x1b]633;NT;END;blind-long-echo;0\x07");
    const auto result = framer.consume(echo);
    QVERIFY(result.visibleBytes.contains(
        QByteArrayLiteral("root# cat /etc/os-release")));
    QVERIFY(result.visibleBytes.contains(QByteArrayLiteral("NAME=Buildroot")));
    QVERIFY(!result.visibleBytes.contains(QByteArrayLiteral("__nvterm")));
    QVERIFY(!result.visibleBytes.contains(QByteArrayLiteral("NT;END")));
    QCOMPARE(result.events.size(), 1);
    QCOMPARE(result.events.first().kind,
             InteractiveStreamEventKind::CommandFinished);
}

void SessionTests::unverifiedLongEchoHidesSuffixAcrossBackspaceRedraw()
{
    InteractiveCommandProfile profile;
    profile.shellIntegration = true;
    profile.requiresStartMarker = false;
    profile.allowUnverifiedPrompt = true;
    InteractiveStreamFramer framer;
    framer.configure(profile);
    framer.reset(33);
    const QByteArray nonce = QByteArrayLiteral("blind-redraw");
    const QByteArray suffix = QByteArrayLiteral(
        ";__nvterm_rc=$?;printf '\\033]633;NT;END;blind-redraw;%d\\007' \"$__nvterm_rc\"");
    framer.beginTransaction(nonce, suffix);

    const qsizetype redrawAt = suffix.indexOf(QByteArrayLiteral("NT;END")) + 4;
    QVERIFY(redrawAt > 4);
    const QByteArray echo = QByteArrayLiteral("root# cat /etc/os-release")
        + suffix.left(redrawAt) + QByteArrayLiteral("\bE")
        + suffix.mid(redrawAt)
        + QByteArrayLiteral("\r\nNAME=Buildroot\r\n")
        + QByteArrayLiteral("\x1b]633;NT;END;blind-redraw;0\x07");
    const qsizetype split = echo.indexOf('\b');
    const auto first = framer.consume(echo.left(split + 1));
    const auto second = framer.consume(echo.mid(split + 1));
    const QByteArray visible = first.visibleBytes + second.visibleBytes;
    QVERIFY(visible.contains(QByteArrayLiteral("root# cat /etc/os-release")));
    QVERIFY(visible.contains(QByteArrayLiteral("NAME=Buildroot")));
    QVERIFY(!visible.contains(QByteArrayLiteral("__nvterm")));
    QVERIFY(!visible.contains(QByteArrayLiteral("NT;EEND")));
    QCOMPARE(second.events.size(), 1);
    QCOMPARE(second.events.first().kind,
             InteractiveStreamEventKind::CommandFinished);
}

void SessionTests::unverifiedLongEchoHidesPartialSuffixBeforeLineRedraw()
{
    InteractiveCommandProfile profile;
    profile.shellIntegration = true;
    profile.requiresStartMarker = false;
    profile.allowUnverifiedPrompt = true;
    InteractiveStreamFramer framer;
    framer.configure(profile);
    framer.reset(34);
    const QByteArray nonce = QByteArrayLiteral("blind-line-redraw");
    const QByteArray suffix = QByteArrayLiteral(
        ";__nvterm_rc=$?;printf '\\033]633;NT;END;blind-line-redraw;%d\\007' \"$__nvterm_rc\"");
    framer.beginTransaction(nonce, suffix);
    const QByteArray promptAndCommand = QByteArrayLiteral(
        "root# cat /etc/os-release");

    const qsizetype redrawAt = suffix.indexOf(QByteArrayLiteral("NT;END")) + 4;
    QVERIFY(redrawAt > 4);
    const QByteArray redrawSuffix = suffix.left(redrawAt)
        + QByteArrayLiteral("\bE") + suffix.mid(redrawAt);
    const QByteArray echo = promptAndCommand + suffix.left(40)
        + QByteArrayLiteral("\r") + promptAndCommand + redrawSuffix
        + QByteArrayLiteral("\r\nNAME=Buildroot\r\n")
        + QByteArrayLiteral("\x1b]633;NT;END;blind-line-redraw;0\x07");
    const auto result = framer.consume(echo);
    QVERIFY(result.visibleBytes.contains(promptAndCommand));
    QVERIFY(result.visibleBytes.contains(QByteArrayLiteral("NAME=Buildroot")));
    QVERIFY(!result.visibleBytes.contains(QByteArrayLiteral("__nvterm")));
    QVERIFY(!result.visibleBytes.contains(QByteArrayLiteral("NT;END")));
    QCOMPARE(result.events.size(), 1);
    QCOMPARE(result.events.first().kind,
             InteractiveStreamEventKind::CommandFinished);
    TerminalCore core(120, 10);
    QVERIFY(core.writeInput(result.visibleBytes).fullyAccepted());
    QVERIFY(core.waitForIdle());
    const auto state = core.terminalState();
    QCOMPARE(QByteArray::fromStdString(state.viewport.at(0).text),
             promptAndCommand);
    QCOMPARE(QByteArray::fromStdString(state.viewport.at(1).text),
             QByteArrayLiteral("NAME=Buildroot"));
}

void SessionTests::cancellingUnverifiedCommandPreservesPartialEcho()
{
    InteractiveCommandProfile profile;
    profile.shellIntegration = true;
    profile.requiresStartMarker = false;
    profile.allowUnverifiedPrompt = true;
    TerminalCore core(80, 24);
    FakeTransport transport;
    QVERIFY(transport.connectToHost());
    SessionInputArbiter arbiter;
    arbiter.bind(&transport, 31);
    InteractiveStreamFramer framer;
    framer.configure(profile);
    framer.reset(31);
    SessionCommandCoordinator coordinator(&arbiter, &framer);
    coordinator.configure(profile);
    coordinator.reset(31);
    SessionInputPump pump(&transport, &core, &framer);
    connect(&pump, &SessionInputPump::interactiveEvent,
            &coordinator, &SessionCommandCoordinator::handleInteractiveEvent);
    connect(&pump, &SessionInputPump::interactiveBytes,
            &coordinator, &SessionCommandCoordinator::handleInteractiveBytes);
    connect(&coordinator, &SessionCommandCoordinator::visibleRemainder,
            &pump, &SessionInputPump::forwardFramerRemainder);
    pump.start();

    CommandExecutionRequest request;
    request.requestId = 302;
    request.command = QByteArrayLiteral("pwd");
    request.executionNonce = QByteArrayLiteral("blind-302");
    QSignalSpy finished(&coordinator, &SessionCommandCoordinator::finished);
    QVERIFY(coordinator.submit(request));
    emit transport.readyRead(QByteArrayLiteral("ordinary;__nv"));
    coordinator.cancel(request.requestId);
    QCOMPARE(finished.count(), 1);
    QVERIFY(core.waitForIdle());
    QByteArray visible;
    for (const auto& line : core.terminalState().viewport)
        visible += QByteArray::fromStdString(line.text);
    QVERIFY(visible.contains(QByteArrayLiteral("ordinary;__nv")));
}

void SessionTests::unverifiedCommandDeadlineReportsTimeout()
{
    InteractiveCommandProfile profile;
    profile.shellIntegration = true;
    profile.requiresStartMarker = false;
    profile.allowUnverifiedPrompt = true;
    FakeTransport transport;
    QVERIFY(transport.connectToHost());
    SessionInputArbiter arbiter;
    arbiter.bind(&transport, 32);
    InteractiveStreamFramer framer;
    framer.configure(profile);
    framer.reset(32);
    SessionCommandCoordinator coordinator(&arbiter, &framer);
    coordinator.configure(profile);
    coordinator.reset(32);
    CommandExecutionRequest request;
    request.requestId = 303;
    request.command = QByteArrayLiteral("pwd");
    request.executionNonce = QByteArrayLiteral("blind-303");
    QSignalSpy finished(&coordinator, &SessionCommandCoordinator::finished);
    QVERIFY(coordinator.submit(request));
    coordinator.expire(request.requestId);
    QCOMPARE(finished.count(), 1);
    const auto result = qvariant_cast<CommandExecutionResult>(finished.first().front());
    QCOMPARE(result.outcome, CommandExecutionOutcome::TimedOut);
    QVERIFY(result.executionMayHaveStarted);
    QVERIFY(!result.terminationConfirmed);
}

void SessionTests::unknownInteractiveProfileDoesNotEnableCommands()
{
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Ssh;
    const auto unverified = ShellIntegration::profileFor(runtime);
    QVERIFY(unverified);
    QVERIFY(unverified->allowUnverifiedPrompt);

    runtime.transport.insert(QStringLiteral("interactiveShellKind"),
                             QStringLiteral("unsupported"));
    QVERIFY(!ShellIntegration::profileFor(runtime));

    runtime.transportKind = TransportKind::LocalShell;
    runtime.transport.insert(QStringLiteral("interactiveShellKind"),
                             QStringLiteral("cmd"));
    QVERIFY(!ShellIntegration::profileFor(runtime));

    runtime.transportKind = TransportKind::Ssh;
    runtime.transport.insert(QStringLiteral("interactiveShellKind"),
                             QStringLiteral("posix"));
    const auto profile = ShellIntegration::profileFor(runtime);
    QVERIFY(profile);
    QCOMPARE(profile->lineEnding, QByteArrayLiteral("\r"));
}

void SessionTests::configuredDevicePromptDoesNotProbeTransport()
{
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Serial;
    QVERIFY(!ShellIntegration::profileFor(runtime));
    runtime.transport.insert(QStringLiteral("interactivePromptPattern"),
                             QStringLiteral("device> $"));
    const auto profile = ShellIntegration::profileFor(runtime);
    QVERIFY(profile);
    QCOMPARE(profile->promptPattern, QStringLiteral("device> $"));

    FakeTransport transport;
    QVERIFY(transport.connectToHost());
    SessionInputArbiter arbiter;
    arbiter.bind(&transport, 12);
    InteractiveStreamFramer framer;
    framer.configure(*profile);
    framer.reset(12);
    SessionCommandCoordinator coordinator(&arbiter, &framer);
    coordinator.reset(12);
    CommandExecutionRequest request;
    request.requestId = 94;
    request.command = QByteArrayLiteral("uname");
    request.executionNonce = QByteArrayLiteral("n94");
    request.expectedPromptGeneration = 1;
    QVERIFY(!coordinator.submit(request));
    QCOMPARE(transport.writes, QByteArray{});
}

void SessionTests::configuredDevicePromptRequiresSilence()
{
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Telnet;
    runtime.transport.insert(QStringLiteral("interactivePromptPattern"),
                             QStringLiteral("device> $"));
    runtime.transport.insert(QStringLiteral("interactiveLineEnding"),
                             QStringLiteral("crlf"));
    const auto profile = ShellIntegration::profileFor(runtime);
    QVERIFY(profile);

    TerminalCore core(80, 24);
    FakeTransport transport;
    QVERIFY(transport.connectToHost());
    SessionInputArbiter arbiter;
    arbiter.bind(&transport, 13);
    InteractiveStreamFramer framer;
    framer.configure(*profile);
    framer.reset(13);
    SessionCommandCoordinator coordinator(&arbiter, &framer);
    coordinator.configure(*profile);
    coordinator.reset(13);
    SessionInputPump pump(&transport, &core, &framer);
    connect(&pump, &SessionInputPump::interactiveEvent,
            &coordinator, &SessionCommandCoordinator::handleInteractiveEvent);
    connect(&pump, &SessionInputPump::interactiveBytes,
            &coordinator, &SessionCommandCoordinator::handleInteractiveBytes);
    pump.start();

    emit transport.readyRead(QByteArrayLiteral("device> "));
    QVERIFY(!coordinator.isPromptReady());
    QTRY_VERIFY_WITH_TIMEOUT(coordinator.isPromptReady(), 500);
    QCOMPARE(transport.writes, QByteArray{});

    CommandExecutionRequest request;
    request.requestId = 95;
    request.command = QByteArrayLiteral("status");
    request.executionNonce = QByteArrayLiteral("n95");
    request.expectedPromptGeneration = coordinator.promptGeneration();
    QVERIFY(coordinator.submit(request));
    QCOMPARE(transport.writes, QByteArrayLiteral("status\r\n"));
}

void SessionTests::terminalSessionUsesConfiguredPromptProfile()
{
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Serial;
    runtime.transport.insert(QStringLiteral("interactivePromptPattern"),
                             QStringLiteral("device> $"));
    TerminalSession session(runtime);
    FakeTransport transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed,
                   TransportKind::Serial);
    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    QVERIFY(!session.commandCoordinator()->isPromptReady());

    emit transport.readyRead(QByteArrayLiteral("device> "));
    QTRY_VERIFY_WITH_TIMEOUT(session.commandCoordinator()->isPromptReady(), 500);
    QCOMPARE(transport.writes, QByteArray{});
}

void SessionTests::localShellPresetsCarryExplicitIntegration()
{
    const auto powerShell = LocalShellProfiles::windowsPowerShell();
    QCOMPARE(powerShell.interactiveShellKind, QStringLiteral("powershell"));
    QVERIFY(powerShell.arguments.contains(QStringLiteral("-NoExit")));
    QVERIFY(powerShell.arguments.join(QLatin1Char(' ')).contains(
        QStringLiteral("633;NT;PROMPT")));

    const auto commandPrompt = LocalShellProfiles::commandPrompt();
    QVERIFY(commandPrompt.interactiveShellKind.isEmpty());
    QVERIFY(!commandPrompt.environment.contains(QStringLiteral("PROMPT")));

#ifndef Q_OS_WIN
    const auto posix = LocalShellProfiles::platformDefault();
    QCOMPARE(posix.interactiveShellKind, QStringLiteral("posix"));
    QVERIFY(posix.environment.contains(QStringLiteral("PROMPT_COMMAND")));
#endif
}

void SessionTests::sshShellEnvironmentRequiresExplicitProfile()
{
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Ssh;
    QVERIFY(ShellIntegration::startupEnvironmentFor(runtime).isEmpty());

    runtime.transport.insert(QStringLiteral("interactiveShellKind"),
                             QStringLiteral("posix"));
    const auto environment = ShellIntegration::startupEnvironmentFor(runtime);
    QVERIFY(environment.contains(QStringLiteral("PROMPT_COMMAND")));
    QVERIFY(environment.value(QStringLiteral("PROMPT_COMMAND"))
        .contains(QStringLiteral("633;NT;PROMPT")));
}

void SessionTests::shellPromptMarkerCompletesCurrentCommand()
{
    InteractiveCommandProfile profile;
    profile.shellIntegration = true;
    InteractiveStreamFramer framer;
    framer.configure(profile);
    framer.reset(14);

    const auto initial = framer.consume(
        QByteArrayLiteral("\x1b]633;NT;PROMPT;1;0\x07user$ "));
    QCOMPARE(initial.events.size(), 1);
    QCOMPARE(initial.events.front().kind,
             InteractiveStreamEventKind::PromptReady);

    framer.beginTransaction(QByteArrayLiteral("tx-1"));
    const auto completed = framer.consume(
        QByteArrayLiteral("done\r\n\x1b]633;NT;PROMPT;2;0\x07user$ "));
    QCOMPARE(completed.visibleBytes, QByteArrayLiteral("done\r\nuser$ "));
    QCOMPARE(completed.events.size(), 1);
    QCOMPARE(completed.events.front().kind,
             InteractiveStreamEventKind::CommandFinishedAtPrompt);
    QCOMPARE(completed.events.front().promptGeneration, quint64(2));
    QCOMPARE(completed.events.front().exitCode, std::optional<int>{0});
}

void SessionTests::clinkHookEnablesCmdOnlyWhenInstalled()
{
#ifndef Q_OS_WIN
    QSKIP("Clink is a Windows CMD integration.");
#else
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString batchPath = directory.filePath(QStringLiteral("clink.bat"));
    QFile batch(batchPath);
    QVERIFY(batch.open(QIODevice::WriteOnly));
    batch.close();
    const QString scriptDir = directory.filePath(QStringLiteral("clink-scripts"));
    QVERIFY(QDir().mkpath(scriptDir));
    QFile script(QDir(scriptDir).filePath(
        QStringLiteral("novaterm_prompt.lua")));
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.close();

    const auto profile = LocalShellProfiles::commandPrompt(directory.path());
    QCOMPARE(profile.interactiveShellKind, QStringLiteral("cmd"));
    QVERIFY(profile.environment.value(QStringLiteral("CLINK_PATH"))
        .contains(scriptDir));

    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::LocalShell;
    runtime.transport.insert(QStringLiteral("interactiveShellKind"),
                             QStringLiteral("cmd"));
    QVERIFY(!ShellIntegration::profileFor(runtime));
    runtime.transport.insert(QStringLiteral("interactiveHookReady"), true);
    QVERIFY(ShellIntegration::profileFor(runtime));
#endif
}

void SessionTests::unconfiguredFramerLeavesTerminalBytesUntouched()
{
    InteractiveStreamFramer framer;
    framer.reset(18);
    const QByteArray output =
        QByteArrayLiteral("text\x1b]633;NT;PROMPT;1;0\x07");
    const auto result = framer.consume(output);
    QCOMPARE(result.visibleBytes, output);
    QVERIFY(result.events.isEmpty());
}

void SessionTests::transportRebindDropsPreviousPromptProfile()
{
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Serial;
    runtime.transport.insert(QStringLiteral("interactivePromptPattern"),
                             QStringLiteral("device> $"));
    TerminalSession session(runtime);
    FakeTransport serial;
    session.attach(&serial, TerminalSession::Ownership::Borrowed,
                   TransportKind::Serial);
    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    session.close(CloseMode::Abort);
    QTRY_COMPARE(session.state(), SessionState::Closed);
    QVERIFY(session.resetForReuse());

    FakeTransport ssh;
    session.attach(&ssh, TerminalSession::Ownership::Borrowed,
                   TransportKind::Ssh);
    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    emit ssh.readyRead(QByteArrayLiteral("device> "));
    QTest::qWait(250);
    QVERIFY(!session.commandCoordinator()->isPromptReady());
}

void SessionTests::devicePasswordAndSplitAlternateScreenAreNotReady()
{
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Telnet;
    runtime.transport.insert(QStringLiteral("interactivePromptPattern"),
                             QStringLiteral("device> $"));
    const auto profile = ShellIntegration::profileFor(runtime);
    QVERIFY(profile);
    InteractiveStreamFramer framer;
    framer.configure(*profile);
    framer.reset(20);

    const auto password = framer.consume(
        QByteArrayLiteral("Password: "));
    QVERIFY(password.events.isEmpty());

    const auto alternateFirst = framer.consume(
        QByteArrayLiteral("\x1b[?104"));
    QVERIFY(alternateFirst.events.isEmpty());
    const auto alternateSecond = framer.consume(
        QByteArrayLiteral("9hdevice> "));
    QVERIFY(std::any_of(alternateSecond.events.cbegin(),
                        alternateSecond.events.cend(),
                        [](const auto& event) {
        return event.kind == InteractiveStreamEventKind::ShellReset;
    }));
    QVERIFY(std::none_of(alternateSecond.events.cbegin(),
                         alternateSecond.events.cend(),
                         [](const auto& event) {
        return event.kind == InteractiveStreamEventKind::PromptCandidate;
    }));
}

void SessionTests::userInputStopsUnsentLongCommandTail()
{
    FakeTransport transport;
    QVERIFY(transport.connectToHost());
    SessionInputArbiter arbiter;
    arbiter.bind(&transport, 21);
    InteractiveStreamFramer framer;
    InteractiveCommandProfile profile;
    profile.shellIntegration = true;
    profile.requiresStartMarker = false;
    framer.configure(profile);
    framer.reset(21);
    SessionCommandCoordinator coordinator(&arbiter, &framer);
    coordinator.configure(profile);
    coordinator.reset(21);
    coordinator.handleInteractiveEvent({
        InteractiveStreamEventKind::PromptReady, 1, std::nullopt});

    CommandExecutionRequest request;
    request.requestId = 200;
    request.command = QByteArray(2048, 'x');
    request.executionNonce = QByteArrayLiteral("long-200");
    request.expectedPromptGeneration = 1;
    QSignalSpy finished(&coordinator, &SessionCommandCoordinator::finished);
    QVERIFY(coordinator.submit(request));
    const qsizetype firstPart = transport.writes.size();
    QVERIFY(firstPart > 0);
    QVERIFY(firstPart < request.command.size());

    arbiter.submitUserInput(QByteArrayLiteral("u"));
    QTest::qWait(30);
    QCOMPARE(transport.writes.size(), firstPart + 1);
    QVERIFY(transport.writes.endsWith('u'));
    QCOMPARE(finished.count(), 1);
    const auto result = qvariant_cast<CommandExecutionResult>(
        finished.first().front());
    QCOMPARE(result.outcome, CommandExecutionOutcome::Cancelled);
    QVERIFY(result.executionMayHaveStarted);
}

void SessionTests::serialAutomaticReconnect()
{
    RuntimeConfig config;
    config.transportKind = TransportKind::Serial;
    config.transport.insert(QStringLiteral("reconnectSeconds"), 1);
    TerminalSession session(config);
    QSignalSpy automaticSuccess(&session, &TerminalSession::automaticReconnectSucceeded);
    QSignalSpy automaticAttempts(&session, &TerminalSession::automaticReconnectAttemptStarted);
    FakeTransport transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed);
    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    transport.simulateRemoteDisconnect();
    QCOMPARE(transport.connectAttempts, 1);
    QTRY_COMPARE_WITH_TIMEOUT(transport.connectAttempts, 2, 2500);
    QTRY_COMPARE(automaticSuccess.count(), 1);
    QCOMPARE(automaticAttempts.count(), 1);
    QTRY_COMPARE(session.state(), SessionState::Running);
    QVERIFY(session.disconnectForReconnect());
    QTest::qWait(1200);
    QCOMPARE(transport.connectAttempts, 2);
    QVERIFY(session.reconnect());
    QTRY_COMPARE(session.state(), SessionState::Running);
    transport.simulateRemoteDisconnect();
    session.close();
    QTest::qWait(1200);
    QCOMPARE(transport.connectAttempts, 3);
    QCOMPARE(automaticSuccess.count(), 1);
    QCOMPARE(automaticAttempts.count(), 1);
}

void SessionTests::automaticReconnectDisabledForZeroAndOtherProtocols()
{
    for (const auto kind : {TransportKind::Serial, TransportKind::Ssh}) {
        RuntimeConfig config;
        config.transportKind = kind;
        config.transport.insert(QStringLiteral("reconnectSeconds"),
                                kind == TransportKind::Serial ? 0 : 1);
        TerminalSession session(config);
        FakeTransport transport;
        session.attach(&transport, TerminalSession::Ownership::Borrowed);
        QVERIFY(session.start());
        QTRY_COMPARE(session.state(), SessionState::Running);
        transport.simulateRemoteDisconnect();
        QTest::qWait(1200);
        QCOMPARE(transport.connectAttempts, 1);
    }
    RuntimeConfig config;
    config.transportKind = TransportKind::Serial;
    config.transport.insert(QStringLiteral("reconnectSeconds"), 1);
    TerminalSession session(config);
    FakeTransport transport;
    transport.failedAttempts = 2;
    session.attach(&transport, TerminalSession::Ownership::Borrowed);
    QVERIFY(!session.start());
    QTRY_COMPARE_WITH_TIMEOUT(transport.connectAttempts, 3, 3500);
    QTRY_COMPARE(session.state(), SessionState::Running);
}

void SessionTests::inputPumpOffsetsPreservePendingSuffix()
{
    TerminalCore core(120, 40);
    FakeTransport transport;
    SessionInputPump pump(&transport, &core);
    pump.start();
    const QByteArray burst(10 * 1024 * 1024, 'x');
    emit transport.readyRead(burst);
    const QByteArray suffix("\r\nPUMP_SUFFIX_OK\r\n");
    emit transport.readyRead(suffix);
    QTRY_COMPARE_WITH_TIMEOUT(pump.statistics().pendingBytes, 0, 15000);
    QCOMPARE(pump.statistics().receivedBytes, quint64(burst.size() + suffix.size()));
    QCOMPARE(pump.statistics().acceptedBytes, pump.statistics().receivedBytes);
    QCOMPARE(pump.statistics().overloadCount, quint64(0));
    QVERIFY(core.waitForIdle());
    const auto state = core.terminalState();
    QVERIFY(std::any_of(state.viewport.begin(), state.viewport.end(), [](const auto& line) {
        return line.text == "PUMP_SUFFIX_OK";
    }));
}

void SessionTests::agentContextJoinsHistorySeamAndBoundsUtf8()
{
    TerminalCore core(10, 3);
    const QByteArray wrapped("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN\r\n");
    QVERIFY(core.writeInput(wrapped).fullyAccepted());
    QVERIFY(core.waitForIdle());
    TerminalContextProvider provider(&core);
    const auto context = provider.context({});
    QVERIFY(std::any_of(context.recentOutput.begin(), context.recentOutput.end(), [](const auto& line) {
        return line.text == "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN";
    }));
    QVERIFY(core.writeInput(QByteArray("\x1b]2;") + QStringLiteral("中文").toUtf8() + '\x07').fullyAccepted());
    QVERIFY(core.waitForIdle());
    const auto bounded = provider.context({0, 4, 1, true});
    QCOMPARE(bounded.title, QStringLiteral("中").toStdString());
    QVERIFY(bounded.truncated);
    TerminalStateCache cache;
    for (NovaTerm::u64 revision = 1; revision <= 2048; ++revision)
        cache.append(revision, std::to_string(revision));
    QCOMPARE(cache.entries().size(), TerminalStateCache::MaxLines);
    QVERIFY(cache.floor() > 0);
}

void SessionTests::agentContextFiltersProgressWrapDuplicatesAndAlternate()
{
    TerminalCore core(10, 6);
    core.resize(10, 6);
    QVERIFY(core.waitForIdle());
    TerminalContextProvider provider(&core);
    const auto feed = [&](const QByteArray& data) {
        return core.writeInput(data).fullyAccepted() && core.waitForIdle();
    };
    const auto contains = [](const auto& lines, const std::string& value) {
        return std::any_of(lines.begin(), lines.end(), [&](const auto& line) {
            return line.text == value;
        });
    };
    QVERIFY(feed("1234567890AB\r\n"));
    auto context = provider.context({});
    QVERIFY(contains(context.recentOutput, "1234567890AB"));
    auto revision = context.revision;
    for (int percent = 1; percent < 100; ++percent) {
        QVERIFY(feed("\r" + QByteArray::number(percent) + "%"));
        context = provider.context({revision, 65536, 256, true});
        QVERIFY(context.recentOutput.empty());
        revision = context.revision;
    }
    QVERIFY(feed("\r\x1b[2Kdone\r\ndone\r\n|\r\n"));
    context = provider.context({revision, 65536, 256, true});
    QCOMPARE(std::count_if(context.recentOutput.begin(), context.recentOutput.end(),
        [](const auto& line) { return line.text == "done"; }), 1);
    QVERIFY(!contains(context.recentOutput, "|"));
    revision = context.revision;
    QVERIFY(feed("\x1b[?1049h\x1b[Htop 1"));
    context = provider.context({revision, 65536, 256, true});
    QVERIFY(context.alternateScreen);
    QVERIFY(context.recentOutput.empty());
    QVERIFY(context.resetRequired);
    for (int frame = 0; frame < 100; ++frame) {
        QVERIFY(feed("\x1b[Htop " + QByteArray::number(frame)));
        context = provider.context({0, 65536, 256, true});
        QVERIFY(context.recentOutput.empty());
        QVERIFY(context.viewport.size() <= 6);
    }
    QVERIFY(feed("\x1b[?1049l\x1b]2;agent-title\x07"));
    context = provider.context({});
    QVERIFY(!context.alternateScreen);
    QCOMPARE(context.title, std::string("agent-title"));
    context = provider.context({0, 12, 2, true});
    std::size_t bytes = context.title.size();
    for (const auto& line : context.viewport) bytes += line.text.size();
    for (const auto& line : context.recentOutput) bytes += line.text.size();
    QVERIFY(bytes <= 12);
    QVERIFY(context.viewport.size() + context.recentOutput.size() <= 2);
    QVERIFY(context.truncated);
}

void SessionTests::lifecycleReachesRunningThenClosed()
{
    // 采纳「1 TerminalView 拥有 1 Session」架构后，会话生命周期由拥有方
    // （生产中是 TerminalView）直接驱动，不经 SessionManager。此用例验证
    // attach→start→Running→close→Closed 的核心生命周期在 TerminalSession
    // 层面自洽。
    RuntimeConfig config;
    config.title = QStringLiteral("test");
    TerminalSession session(config);
    auto* transport = new FakeTransport;
    session.attach(transport);
    QSignalSpy states(&session, &TerminalSession::stateChanged);
    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    session.close(CloseMode::Graceful);
    QTRY_COMPARE(session.state(), SessionState::Closed);
    QVERIFY(states.size() >= 2);
}

void SessionTests::manualDisconnectKeepsTransportReconnectable_data()
{
    QTest::addColumn<bool>("useEnter");
    QTest::newRow("button") << false;
    QTest::newRow("enter") << true;
}

void SessionTests::manualDisconnectKeepsTransportReconnectable()
{
    QFETCH(bool, useEnter);
    RuntimeConfig config;
    config.transportKind = TransportKind::Ssh;
    TerminalSession session(config);
    auto* transport = new FakeTransport;
    session.attach(transport, TerminalSession::Ownership::Adopt,
                   TransportKind::Ssh);

    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    QVERIFY(session.disconnectForReconnect());
    QCOMPARE(session.state(), SessionState::Failed);
    QCOMPARE(session.transport(), transport);
    QVERIFY(!transport->isConnected());
    QVERIFY(session.canReconnect());

    session.writeUserInput(QByteArrayLiteral("ignored-while-disconnected"));
    QVERIFY(transport->writes.isEmpty());
    if (useEnter) {
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier,
                        QStringLiteral("\r"));
        session.core()->processKeyPress(&enter);
    } else {
        QVERIFY(session.reconnect());
    }
    QTRY_COMPARE(session.state(), SessionState::Running);
    QCOMPARE(transport->connectAttempts, 2);
    session.writeUserInput(QByteArrayLiteral("input-after-reconnect"));
    QCOMPARE(transport->writes, QByteArrayLiteral("input-after-reconnect"));
}

void SessionTests::enterReconnectsBySessionType()
{
    const QList<TransportKind> kinds{
        TransportKind::LocalShell,
        TransportKind::Ssh,
        TransportKind::Serial,
    };

    for (const TransportKind kind : kinds) {
        RuntimeConfig config;
        config.transportKind = kind;
        TerminalSession session(config);
        auto* transport = new FakeTransport;
        session.attach(transport);
        QVERIFY(session.start());
        QTRY_COMPARE(session.state(), SessionState::Running);

        transport->simulateRemoteDisconnect();
        QCOMPARE(session.state(), SessionState::Failed);
        QVERIFY(session.canReconnect());

        // 使用真实键盘入口验证 Enter 被 TerminalCore 编码后由会话层拦截。
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier,
                        QStringLiteral("\r"));
        session.core()->processKeyPress(&enter);
        QTRY_COMPARE(session.state(), SessionState::Running);
        QCOMPARE(transport->connectAttempts, 2);
        QVERIFY(!transport->readPaused);
        QCOMPARE(session.statistics().reconnectCount, quint64{1});
    }
}

void SessionTests::customSessionWithoutCapabilityDoesNotReconnect()
{
    RuntimeConfig config;
    config.transportKind = TransportKind::Custom;
    TerminalSession session(config);
    auto* transport = new FakeTransport(false);
    session.attach(transport);
    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);

    transport->simulateRemoteDisconnect();
    QCOMPARE(session.state(), SessionState::Failed);
    QVERIFY(!session.canReconnect());
    QVERIFY(!session.reconnect());
    QCOMPARE(transport->connectAttempts, 1);
}

void SessionTests::runtimeConfigIsSnapshot()
{
    RuntimeConfig original;
    original.profileId = QStringLiteral("profile-a");
    original.transport.insert(QStringLiteral("host"), QStringLiteral("one"));
    TerminalSession session(original);
    original.transport.insert(QStringLiteral("host"), QStringLiteral("two"));
    QCOMPARE(session.runtimeConfig().transport.value(QStringLiteral("host")).toString(),
             QStringLiteral("one"));
}

void SessionTests::restoreMetadataRoundTrip()
{
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    SessionStore store(directory.filePath(QStringLiteral("sessions.json")));
    SessionRestoreMetadata source;
    source.sessionId = QUuid::createUuid();
    source.profileId = QStringLiteral("local-default");
    source.overrides.insert(QStringLiteral("workingDirectory"), QStringLiteral("C:/tmp"));
    source.runtimeSnapshot.profileId = source.profileId;
    source.runtimeSnapshot.transportKind = TransportKind::LocalShell;
    QString error;
    QVERIFY2(store.save({source}, &error), qPrintable(error));
    const auto restored = store.load(&error);
    QCOMPARE(restored.size(), 1);
    QCOMPARE(restored.first().sessionId, source.sessionId);
    QCOMPARE(restored.first().profileId, source.profileId);
    QCOMPARE(restored.first().overrides, source.overrides);
}

void SessionTests::persistentStoresRejectSecrets()
{
    MemoryProfileStore profiles;
    ConnectionProfile profile;
    profile.id = QStringLiteral("ssh-test");
    profile.settings.insert(QStringLiteral("password"), QStringLiteral("secret"));
    QString error;
    QVERIFY(!profiles.save(profile, &error));
    QVERIFY(!error.isEmpty());

    QTemporaryDir directory;
    SessionStore sessions(directory.filePath(QStringLiteral("sessions.json")));
    SessionRestoreMetadata metadata;
    metadata.sessionId = QUuid::createUuid();
    metadata.overrides.insert(QStringLiteral("token"), QStringLiteral("secret"));
    QVERIFY(!sessions.save({metadata}, &error));

    MemoryCredentialStore credentials;
    QVERIFY(credentials.put(QStringLiteral("credential-ref"), QByteArrayLiteral("secret")));
    QCOMPARE(credentials.get(QStringLiteral("credential-ref")).value(),
             QByteArrayLiteral("secret"));
}

void SessionTests::reconnectBumpsGenerationAndKeepsHandlersLive()
{
    RuntimeConfig config;
    config.transportKind = TransportKind::Telnet;
    TerminalSession session(config);
    auto* transport = new FakeTransport;
    session.attach(transport, TerminalSession::Ownership::Adopt,
                   TransportKind::Telnet);

    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    const quint64 firstGeneration = session.statistics().generation;
    QVERIFY(firstGeneration > 0);

    // 重连必须自增世代号，并重建带新世代号的信号接线。
    QVERIFY(session.reconnect());
    QTRY_COMPARE(session.state(), SessionState::Running);
    QVERIFY(session.statistics().generation > firstGeneration);

    // 重接线的回归风险是处理器失效：新世代下的远端断开仍须被识别。
    transport->simulateRemoteDisconnect();
    QCOMPARE(session.state(), SessionState::Failed);
}

void SessionTests::structuredTransportErrorSetsSessionCategory()
{
    RuntimeConfig config;
    config.transportKind = TransportKind::Ssh;
    TerminalSession session(config);
    auto* transport = new FakeTransport;
    session.attach(transport, TerminalSession::Ownership::Adopt,
                   TransportKind::Ssh);
    QSignalSpy errors(&session, &TerminalSession::sessionError);

    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);

    // transportError 提供分类，errorOccurred 统一上报：只应产生一条 sessionError。
    transport->simulateStructuredError(TransportErrorCategory::Authentication,
                                       QStringLiteral("auth failed"), false);
    QCOMPARE(errors.count(), 1);
    auto error = qvariant_cast<SessionError>(errors.takeFirst().constFirst());
    QCOMPARE(error.category, SessionErrorCategory::Authentication);
    QCOMPARE(error.code, 42);
    QCOMPARE(error.message, QStringLiteral("auth failed"));
    QVERIFY(!error.retryable);

    // 只发文本错误的后端回落到 Io，保持既有行为。
    transport->simulatePlainError(QStringLiteral("plain failure"));
    QCOMPARE(errors.count(), 1);
    error = qvariant_cast<SessionError>(errors.takeFirst().constFirst());
    QCOMPARE(error.category, SessionErrorCategory::Io);

    // 分类不得粘连到下一条错误。
    transport->simulateStructuredError(TransportErrorCategory::HostKey,
                                       QStringLiteral("host key changed"), false);
    QCOMPARE(errors.count(), 1);
    error = qvariant_cast<SessionError>(errors.takeFirst().constFirst());
    QCOMPARE(error.category, SessionErrorCategory::HostKey);
}

// 回归："保存的密码认证 SSH 历史会话重连报凭据不可用"。
// 历史记录只持久化 credentialRef，凭据必须能跨进程存活；旧实现在非 Windows
// 平台用纯内存存储，重启后必然查不到，于是每次重连都报凭据不可用。
//
// 非 Windows 的持久化后端是 freedesktop Secret Service：这里直接读写真实密钥环
// （没有会话总线/密钥环时跳过），用"新建一个 store 实例"代表应用重启。测试条目
// 带 service=NovaTerm、novaterm-ref=novaterm-test-* 属性，结束时删除，便于清理。
void SessionTests::secretServiceCredentialStoreSurvivesRestart()
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    if (!SecretServiceCredentialStore::isServiceAvailable())
        QSKIP("no freedesktop Secret Service on the session bus");

    const QString reference =
        QStringLiteral("novaterm-test-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));

    {
        SecretServiceCredentialStore store;
        QVERIFY(store.isPersistent());
        QVERIFY(store.put(reference, QByteArrayLiteral("s3cret")));
        // 同一个 store 立刻读回，覆盖"写入后 GetSecret 可解析"的编解码路径。
        const auto immediate = store.get(reference);
        QVERIFY(immediate.has_value());
        QCOMPARE(*immediate, QByteArrayLiteral("s3cret"));
        // 覆盖写：同一个引用二次 put 不能留下旧条目（否则 get 可能读到过期密码）。
        QVERIFY(store.put(reference, QByteArrayLiteral("r0tated")));
    }

    // 新实例代表应用重启：密钥环里的条目必须仍能取到。
    {
        SecretServiceCredentialStore restarted;
        const auto secret = restarted.get(reference);
        QVERIFY(secret.has_value());
        QCOMPARE(*secret, QByteArrayLiteral("r0tated"));
        QVERIFY(restarted.remove(reference));
        QVERIFY(!restarted.get(reference).has_value());
    }
    {
        SecretServiceCredentialStore afterRemoval;
        QVERIFY(!afterRemoval.get(reference).has_value());
    }

    // 空引用/空凭据一律拒绝，避免产生取不回来的条目。
    SecretServiceCredentialStore store;
    QVERIFY(!store.put(QString(), QByteArrayLiteral("secret")));
    QVERIFY(!store.put(reference, QByteArray()));
    QVERIFY(!store.get(QString()).has_value());
    QVERIFY(!store.remove(QString()));

    // 工厂在非 Windows 上必须给出可持久化实现（有密钥环时），防止再次退化成
    // "重启即失效"。
    const auto created = createCredentialStore();
    QVERIFY(created != nullptr);
    QVERIFY(created->isPersistent());
    QVERIFY(dynamic_cast<SecretServiceCredentialStore*>(created.get()) != nullptr);
#else
    QSKIP("Secret Service backend is Unix-only");
#endif
}

QTEST_MAIN(SessionTests)
#include "SessionTests.moc"
