#include "core/terminal/TerminalCore.h"
#include "credential/CredentialStore.h"
#include "profile/ProfileStore.h"
#include "session/InteractiveStreamFramer.h"
#include "session/SessionCommandCoordinator.h"
#include "session/ShellIntegration.h"
#include "session/LocalShellProfile.h"
#include "session/LocalSessionScriptProvider.h"
#include "session/SessionInputArbiter.h"
#include "session/SessionTerminalController.h"
#include "session/SessionStore.h"
#include "session/TerminalSession.h"
#include "session/SessionInputPump.h"
#include "service/HistoryRecordingBinding.h"
#include "recording/HistoryService.h"
#include "transport/ITransport.h"
#include "transport/SerialTransport.h"
#include "transport/SerialDriverReceiveQueue.h"
#include "transport/SerialIoState.h"
#include "session/transfer/SerialFileTransferController.h"
#include "filetransfer/XmodemEngine.h"
#include "SerialLrzszTestSupport.h"
#include <QSocketNotifier>
#include <QTimer>
#ifdef Q_OS_LINUX
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#endif

#include <QSignalSpy>
#include <QStringList>
#include <QKeyEvent>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace {
template<class Capture, class Read, class Matches>
bool sampleCurrentCapture(Capture& capture, Read read, Matches matches)
{
    if (capture && matches(*capture)) return true;
    auto candidate = read();
    if (!candidate || !matches(*candidate)) return false;
    capture = std::move(candidate);
    return true;
}
}

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
    void recordsOnceAcrossBackpressure();
    void recordingLifecycleFollowsConnectionGeneration();
    void recordingBindingFollowsSessionIdentityReuse();
    void serialIoPausedDeliveryKeepsBoundedBytes();
    void serialIoReconnectRejectsOldGeneration();
    void serialIoOriginalGenerationRejectsWriteAndClear();
    void serialTransportOriginalGenerationRejectsWriteAndClear();
    void serialTransferOldIoGenerationCannotAffectNewConnection_data();
    void serialTransferOldIoGenerationCannotAffectNewConnection();
    void serialIoClearPreservesBarrierBeforeNewWrites();
    void serialIoNotificationsAreCoalesced();
    void serialIoStopRejectsFurtherWork();
    void serialIoFailedOpenAndImmediateDestructionReclaimWorkers();
    void serialIoReconnectInsideReadCallbackKeepsReceiving();
    void serialIoClosedBackendCannotReportFalseWriteDrain();
    void serialDriverReceiveQueueRequiresActualCapacity_data();
    void serialDriverReceiveQueueRequiresActualCapacity();
    void serialDriverReceiveQueueIsInitializedAgainAfterReopen();
    void transportWarningsStayNonFatalAndRejectOldAttachments();
    void serialAutomaticReconnect();
    void serialPendingReconnectReflectsActualState_data();
    void serialPendingReconnectReflectsActualState();
    void realSerialSessionUsesExistingPumpForFileTransfer_data();
    void realSerialSessionUsesExistingPumpForFileTransfer();
    void serialLrzszSizeMatrix_data();
    void serialLrzszSizeMatrix();
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
    void reconnectResetsTerminalParser_data();
    void reconnectResetsTerminalParser();
    void structuredTransportErrorSetsSessionCategory();
    void agentContextFiltersProgressWrapDuplicatesAndAlternate();
    void agentContextCaptureKeepsSuccessfulQtryResult_data();
    void agentContextCaptureKeepsSuccessfulQtryResult();
    void agentContextClearDropsPreviousSummary();
    void agentContextRejectsPreviousGenerationPublishedFallback();
    void inputPumpOffsetsPreservePendingSuffix();
    void fileTransferInputUsesSinglePumpAndIndependentPause();
    void userInputPreemptsPartialMcpWrite();
    void terminalProtocolReplyDoesNotPreemptMcpLease();
    void newInputInvalidatesLegacyCommandReadiness();
    void emptyAgentBatchPreservesCommandReadiness();
    void cacheInvalidationRejectsDeferredAgentInput_data();
    void cacheInvalidationRejectsDeferredAgentInput();
    void deferredInputAdmissionRejectsOldCacheGeneration();
    void cacheClearCanOverlapAnAdmittedTransportWrite();
    void shellIntegrationIsIndependentOfTransport_data();
    void shellIntegrationIsIndependentOfTransport();
    void integratedCtrlCRecoversReadinessAtNewPrompt_data();
    void integratedCtrlCRecoversReadinessAtNewPrompt();
    void terminalRawInputPreservesConfiguredEndingAndKeys_data();
    void terminalRawInputPreservesConfiguredEndingAndKeys();
    void freeTerminalInputTracksOnlyIntegratedEvidence();
    void recentCommandExpiresWithoutDiscardingActiveCommand();
    void freeInputKeepsTrailingTextOutOfCommandReadiness_data();
    void freeInputKeepsTrailingTextOutOfCommandReadiness();
    void nonemptyAgentBatchInvalidatesPreWritePrompt_data();
    void nonemptyAgentBatchInvalidatesPreWritePrompt();
    void emptyAgentBatchKeepsLatestPromptEvidence_data();
    void emptyAgentBatchKeepsLatestPromptEvidence();
    void queuedUserInputPreemptsLaterAgentBatch();
    void fileTransferLeaseGatesAllInputAndResets();
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
    void serialWithoutPromptCompletesByIdleQuiet();
    void serialIdleCompletionNoOutputFallsBackToDone();
    void serialIdleCompletionKeepsEchoWhenRemoteEchoDisabled();
    void serialIdleCompletionTimeoutWinsOverLongQuiet();
    void serialIdleCompletionPreemptedByUserInput();
    void configuredDevicePromptRequiresSilence();
    void terminalSessionUsesConfiguredPromptProfile();
    void attachmentSnapshotConfiguresDeviceCommands();
    void serialCommandSettingsRejectInvalidInput();
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

/** @brief 只构造交接状态；真实的门面交付函数与 Qt 信号保持不变。 */
class SerialTransportIoTestAccess
{
public:
    /** @brief 弱引用过期证明门面、worker 和线程均已释放交接状态。 */
    static std::weak_ptr<NovaTerm::SerialIoState> ioLifetime(const SerialTransport& transport)
    { return transport._state; }
    static void openMemory(SerialTransport& transport)
    {
        const auto generation = transport._state->requestOpen();
        transport._state->opened(generation);
        transport._connected = true;
    }
    static void receive(SerialTransport& transport, const QByteArray& bytes)
    {
        transport._state->receive(transport._state->snapshot().generation, bytes);
        if (transport._state->armReceiveNotification())
            QMetaObject::invokeMethod(&transport, &SerialTransport::deliverInput, Qt::QueuedConnection);
    }
    static void closeBackend(SerialTransport& transport)
    { transport._state->closed(transport._state->snapshot().generation); }
};

void SessionTests::serialIoClosedBackendCannotReportFalseWriteDrain()
{
    SerialConfig config; config.portName = QStringLiteral("memory-only-not-opened");
    SerialTransport transport(config);
    SerialTransportIoTestAccess::openMemory(transport);
    QCOMPARE(transport.tryWriteBounded(QByteArrayLiteral("not-written")), qint64(11));
    // I/O 关闭已完成，但 GUI 状态通知尚未消费；不能把丢弃当作写完成。
    SerialTransportIoTestAccess::closeBackend(transport);
    QCOMPARE(transport.pendingWriteBytes(), qint64(-1));
    QVERIFY(!transport.isConnected());
}

void SessionTests::serialIoReconnectInsideReadCallbackKeepsReceiving()
{
    SerialConfig config;
    config.portName = QStringLiteral("memory-only-not-opened");
    SerialTransport transport(config);
    SerialTransportIoTestAccess::openMemory(transport);
    QSignalSpy received(&transport, &ITransport::readyRead);
    bool first = true;
    connect(&transport, &ITransport::readyRead, this, [&](const QByteArray&) {
        if (!first) return;
        first = false;
        SerialTransportIoTestAccess::openMemory(transport);
        SerialTransportIoTestAccess::receive(transport, QByteArrayLiteral("new-generation"));
    });
    SerialTransportIoTestAccess::receive(transport, QByteArrayLiteral("old-generation"));
    QTRY_COMPARE_WITH_TIMEOUT(received.count(), 2, 1000);
    QCOMPARE(received.at(0).at(0).toByteArray(), QByteArrayLiteral("old-generation"));
    QCOMPARE(received.at(1).at(0).toByteArray(), QByteArrayLiteral("new-generation"));
}

void SessionTests::serialIoPausedDeliveryKeepsBoundedBytes()
{
    NovaTerm::SerialIoState state(8, 16);
    const auto generation = state.requestOpen();
    QVERIFY(state.opened(generation));
    state.setDeliveryPaused(true);
    QVERIFY(state.receive(generation, QByteArrayLiteral("abcdefgh")));
    QVERIFY(!state.receive(generation, QByteArrayLiteral("x")));
    QCOMPARE(state.receiveRoom(generation), qint64(0));
    QVERIFY(state.takeReceived().isEmpty());
    QVERIFY(!state.armReceiveNotification());
    state.setDeliveryPaused(false);
    QCOMPARE(state.takeReceived(3), QByteArrayLiteral("abc"));
    QCOMPARE(state.takeReceived(), QByteArrayLiteral("defgh"));
    const auto statistics = state.snapshot();
    QCOMPARE(statistics.receivedBytes, quint64(8));
    QCOMPARE(statistics.deliveredBytes, quint64(8));
    QCOMPARE(statistics.receiveHighWater, quint64(8));
}

void SessionTests::serialIoReconnectRejectsOldGeneration()
{
    NovaTerm::SerialIoState state;
    const auto first = state.requestOpen();
    QVERIFY(state.opened(first));
    QVERIFY(state.receive(first, QByteArrayLiteral("old-rx")));
    QCOMPARE(state.queueWrite(QByteArrayLiteral("old-tx")), qint64(6));
    const auto oldWrite = state.takeWrite(first, 6);
    const auto second = state.requestOpen();
    QVERIFY(second != first);
    QVERIFY(state.opened(second));
    QVERIFY(!state.receive(first, QByteArrayLiteral("late")));
    QVERIFY(state.takeReceived().isEmpty());
    QCOMPARE(state.pendingWriteBytes(), qint64(0));
    QCOMPARE(state.completeWrite(first, oldWrite.epoch, 6), qint64(0));
    QVERIFY(state.receive(second, QByteArrayLiteral("new")));
    QCOMPARE(state.takeReceived(), QByteArrayLiteral("new"));
}

void SessionTests::serialIoOriginalGenerationRejectsWriteAndClear()
{
    NovaTerm::SerialIoState state;
    const auto oldGeneration = state.requestOpen();
    QVERIFY(state.opened(oldGeneration));
    const auto generation = state.requestOpen();
    QVERIFY(state.opened(generation));
    QCOMPARE(state.queueWrite(QByteArrayLiteral("new"), 16, generation), qint64(3));
    const auto before = state.snapshot();
    QCOMPARE(state.queueWrite(QByteArrayLiteral("payload"), 16, oldGeneration), qint64(-1));
    QCOMPARE(state.queueWrite(QByteArrayLiteral("CAN"), 16, oldGeneration), qint64(-1));
    QVERIFY(!state.requestClear(oldGeneration));
    QCOMPARE(state.snapshot().writeEpoch, before.writeEpoch);
    QCOMPARE(state.takeWrite(generation, 16).bytes, QByteArrayLiteral("new"));
}

void SessionTests::serialTransportOriginalGenerationRejectsWriteAndClear()
{
    SerialConfig config; config.portName = QStringLiteral("memory-only-not-opened");
    SerialTransport transport(config);
    SerialTransportIoTestAccess::openMemory(transport);
    const auto oldGeneration = transport.ioStatistics().generation;
    SerialTransportIoTestAccess::openMemory(transport);
    const auto generation = transport.ioStatistics().generation;
    QCOMPARE(transport.tryWriteBounded(QByteArrayLiteral("new"), 16, generation), qint64(3));
    const auto epoch = transport.ioStatistics().writeEpoch;
    QCOMPARE(transport.pendingWriteBytes(oldGeneration), qint64(-1));
    QCOMPARE(transport.tryWriteBounded(QByteArrayLiteral("CAN"), 16, oldGeneration), qint64(-1));
    QVERIFY(!transport.clearPendingOutput(oldGeneration));
    QCOMPARE(transport.ioStatistics().writeEpoch, epoch);
    QCOMPARE(transport.pendingWriteBytes(generation), qint64(3));
}

void SessionTests::serialTransferOldIoGenerationCannotAffectNewConnection_data()
{
    QTest::addColumn<int>("action");
    QTest::newRow("cancel") << 0;
    QTest::newRow("drive") << 1;
    QTest::newRow("disconnect-required") << 2;
}

void SessionTests::serialTransferOldIoGenerationCannotAffectNewConnection()
{
    QFETCH(int, action);
    TerminalCore core(80, 24);
    FakeTransport initial;
    SerialConfig config; config.portName = QStringLiteral("memory-only-not-opened");
    SerialTransport serial(config);
    TerminalSession session(&core);
    session.attach(&initial, TerminalSession::Ownership::Borrowed);
    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    SerialTransportIoTestAccess::openMemory(serial);
    session.attach(&serial, TerminalSession::Ownership::Borrowed, TransportKind::Serial);
    QVERIFY(session.canTransferFiles());
    QTemporaryDir directory; QVERIFY(directory.isValid());
    auto* controller = session.serialFileTransfer(); QVERIFY(controller);
    SerialTransferRequest request;
    request.protocol = SerialTransferProtocol::XmodemCrc;
    request.direction = NovaTerm::FileTransfer::Direction::Receive;
    request.destination = directory.filePath(QStringLiteral("receive.bin"));
    QVERIFY(controller->start(request));
    QTRY_VERIFY(!controller->progress().preparing);
    SerialTransportIoTestAccess::openMemory(serial);
    const auto generation = serial.ioStatistics().generation;
    QCOMPARE(serial.tryWriteBounded(QByteArrayLiteral("new")), qint64(3));
    const auto epoch = serial.ioStatistics().writeEpoch;
    if (action == 0) controller->cancel();
    else if (action == 1) controller->notifyWritable();
    else emit controller->disconnectRequired();
    QVERIFY(serial.isConnected());
    QCOMPARE(serial.ioStatistics().generation, generation);
    QCOMPARE(serial.ioStatistics().writeEpoch, epoch);
    QCOMPARE(serial.pendingWriteBytes(), qint64(3));
    controller->abort(QStringLiteral("cleanup"));
}

void SessionTests::serialIoClearPreservesBarrierBeforeNewWrites()
{
    NovaTerm::SerialIoState state(16, 16);
    const auto generation = state.requestOpen();
    QVERIFY(state.opened(generation));
    QCOMPARE(state.queueWrite(QByteArrayLiteral("0123456789abcdef")), qint64(16));
    QCOMPARE(state.queueWrite(QByteArrayLiteral("x")), qint64(0));
    const auto oldWrite = state.takeWrite(generation, 8);
    QCOMPARE(state.pendingWriteBytes(), qint64(16));
    QCOMPARE(state.completeWrite(generation, oldWrite.epoch, 4), qint64(4));
    QCOMPARE(state.pendingWriteBytes(), qint64(12));
    QVERIFY(state.requestClear());
    const auto clear = state.snapshot();
    QCOMPARE(state.pendingWriteBytes(), qint64(5)); // 4 个在途字节 + 屏障
    QCOMPARE(state.queueWrite(QByteArrayLiteral("CAN")), qint64(3));
    QVERIFY(state.takeWrite(generation, 8).bytes.isEmpty());
    QVERIFY(!state.completeClear(generation, oldWrite.epoch));
    QVERIFY(state.completeClear(generation, clear.writeEpoch));
    QCOMPARE(state.pendingWriteBytes(), qint64(3));
    const auto next = state.takeWrite(generation, 8);
    QCOMPARE(next.bytes, QByteArrayLiteral("CAN"));
    QCOMPARE(state.completeWrite(generation, oldWrite.epoch, 4), qint64(0));
    QCOMPARE(state.pendingWriteBytes(), qint64(3));
    QCOMPARE(state.completeWrite(generation, next.epoch, 3), qint64(3));
    QCOMPARE(state.pendingWriteBytes(), qint64(0));
}

void SessionTests::serialIoNotificationsAreCoalesced()
{
    NovaTerm::SerialIoState state;
    const auto generation = state.requestOpen();
    QVERIFY(state.opened(generation));
    QVERIFY(state.armNoticeNotification());
    QVERIFY(!state.armNoticeNotification());
    QVERIFY(state.takeNotices().opened);
    QVERIFY(state.receive(generation, QByteArrayLiteral("a")));
    QVERIFY(state.armReceiveNotification());
    QVERIFY(state.receive(generation, QByteArrayLiteral("b")));
    QVERIFY(!state.armReceiveNotification());
    QCOMPARE(state.takeReceived(), QByteArrayLiteral("ab"));
    QVERIFY(!state.finishReceiveNotification());
    QVERIFY(state.armWorkerWake());
    QVERIFY(!state.armWorkerWake());
    // 换代不能解除已排队通知，否则快速重连会积累无界 queued 事件。
    state.requestOpen();
    QVERIFY(!state.armWorkerWake());
    state.consumeWorkerWake();
    QVERIFY(state.armWorkerWake());
}

void SessionTests::serialIoStopRejectsFurtherWork()
{
    NovaTerm::SerialIoState state;
    const auto generation = state.requestOpen();
    QVERIFY(state.opened(generation));
    QVERIFY(state.receive(generation, QByteArrayLiteral("held")));
    QCOMPARE(state.queueWrite(QByteArrayLiteral("held")), qint64(4));
    state.requestStop();
    QVERIFY(state.snapshot().stopping);
    QCOMPARE(state.snapshot().receiveBytes, qint64(0));
    QCOMPARE(state.pendingWriteBytes(), qint64(0));
    QVERIFY(!state.receive(generation, QByteArrayLiteral("late")));
    QCOMPARE(state.queueWrite(QByteArrayLiteral("late")), qint64(-1));
    QCOMPARE(state.requestOpen(), quint64(0));
}

void SessionTests::serialIoFailedOpenAndImmediateDestructionReclaimWorkers()
{
    const auto baseline = SerialTransport::activeIoWorkerCount();
    std::vector<std::weak_ptr<NovaTerm::SerialIoState>> lifetimes;
    SerialConfig config;
    config.portName = QStringLiteral("novaterm-no-such-serial-port");
    for (int iteration = 0; iteration < 8; ++iteration) {
        SerialTransport transport(config);
        lifetimes.push_back(SerialTransportIoTestAccess::ioLifetime(transport));
        QSignalSpy errors(&transport, &ITransport::errorOccurred);
        bool publishedOnGui = false;
        connect(&transport, &ITransport::errorOccurred, this, [&] {
            publishedOnGui = QThread::currentThread() == thread();
        });
        QVERIFY(transport.connectToHost());
        QTRY_COMPARE(errors.count(), 1);
        QVERIFY(publishedOnGui);
        QVERIFY(!transport.isConnected());
        QVERIFY(!transport.hasPendingDisconnect());
        QVERIFY(transport.connectToHost());
        transport.disconnect();
        QTRY_VERIFY(!transport.hasPendingDisconnect());
        QVERIFY(!transport.isConnected());
    }
    for (int iteration = 0; iteration < 8; ++iteration) {
        auto transport = std::make_unique<SerialTransport>(config);
        lifetimes.push_back(SerialTransportIoTestAccess::ioLifetime(*transport));
        QVERIFY(transport->connectToHost());
        transport.reset(); // start 投递尚未执行时也必须能关闭 worker
    }
    // 待启动线程尚未计入 active；逐个等待所有者释放，不能只看瞬时计数。
    const auto allReleased = [&] {
        return std::all_of(lifetimes.cbegin(), lifetimes.cend(),
                           [](const auto& lifetime) { return lifetime.expired(); });
    };
    QTRY_VERIFY_WITH_TIMEOUT(allReleased(), 5000);
    QCOMPARE(SerialTransport::activeIoWorkerCount(), baseline);
}

namespace {
/** @brief 只替代 Win32 驱动调用，容量判定使用生产实现。 */
struct FakeSerialQueueDriver
{
    bool acceptsRequest{true};
    bool reportsCapacity{true};
    std::uint32_t capacityAfterRequest{262144};
    std::uint32_t currentCapacity{4096};
    std::uint32_t requestError{0};
    std::uint32_t queryError{0};
    int initializationCount{0};

    bool request(std::uint32_t receiveBytes, std::uint32_t transmitBytes,
                 std::uint32_t& error)
    {
        ++initializationCount;
        if (receiveBytes != 262144 || transmitBytes != 4096) {
            error = 87;
            return false;
        }
        error = requestError;
        if (acceptsRequest) currentCapacity = capacityAfterRequest;
        return acceptsRequest;
    }

    bool query(std::uint32_t& actualBytes, std::uint32_t& error) const
    {
        error = queryError;
        actualBytes = reportsCapacity ? currentCapacity : 0;
        return reportsCapacity;
    }
};
}

void SessionTests::serialDriverReceiveQueueRequiresActualCapacity_data()
{
    QTest::addColumn<bool>("accepts");
    QTest::addColumn<bool>("reports");
    QTest::addColumn<quint32>("actual");
    QTest::addColumn<quint32>("requestError");
    QTest::addColumn<quint32>("queryError");
    QTest::addColumn<bool>("verified");
    QTest::newRow("expanded") << true << true << quint32(262144)
        << quint32(0) << quint32(0) << true;
    QTest::newRow("larger") << true << true << quint32(1048576)
        << quint32(0) << quint32(0) << true;
    QTest::newRow("accepted-but-ignored") << true << true << quint32(4096)
        << quint32(0) << quint32(0) << false;
    QTest::newRow("capacity-not-reported") << true << true << quint32(0)
        << quint32(0) << quint32(0) << false;
    QTest::newRow("request-rejected") << false << true << quint32(4096)
        << quint32(5) << quint32(0) << false;
    QTest::newRow("query-failed") << true << false << quint32(0)
        << quint32(0) << quint32(6) << false;
    QTest::newRow("both-failed") << false << false << quint32(0)
        << quint32(5) << quint32(6) << false;
    QTest::newRow("already-large-request-rejected") << false << true << quint32(262144)
        << quint32(5) << quint32(0) << true;
}

void SessionTests::serialDriverReceiveQueueRequiresActualCapacity()
{
    QFETCH(bool, accepts);
    QFETCH(bool, reports);
    QFETCH(quint32, actual);
    QFETCH(quint32, requestError);
    QFETCH(quint32, queryError);
    QFETCH(bool, verified);
    FakeSerialQueueDriver driver;
    driver.acceptsRequest = accepts;
    driver.reportsCapacity = reports;
    driver.capacityAfterRequest = actual;
    driver.currentCapacity = accepts ? 4096 : actual;
    driver.requestError = requestError;
    driver.queryError = queryError;
    const auto result = NovaTerm::initializeSerialDriverReceiveQueue(driver);
    QCOMPARE(result.verified(), verified);
    QCOMPARE(result.requestAccepted, accepts);
    QCOMPARE(result.capacityQueried, reports);
    QCOMPARE(result.actualBytes, actual);
    QCOMPARE(result.requestError, requestError);
    QCOMPARE(result.queryError, queryError);
}

void SessionTests::serialDriverReceiveQueueIsInitializedAgainAfterReopen()
{
    FakeSerialQueueDriver driver;
    QVERIFY(NovaTerm::initializeSerialDriverReceiveQueue(driver).verified());
    // 重开后驱动恢复默认值，不能沿用上一连接的核验结果。
    driver.currentCapacity = 4096;
    const auto reopened = NovaTerm::initializeSerialDriverReceiveQueue(driver);
    QVERIFY(reopened.verified());
    QCOMPARE(reopened.actualBytes, std::uint32_t(262144));
    QCOMPARE(driver.initializationCount, 2);
}

void SessionTests::transportWarningsStayNonFatalAndRejectOldAttachments()
{
    TerminalCore core(80, 24);
    FakeTransport first;
    FakeTransport second;
    TerminalSession session(&core);
    session.attach(&first, TerminalSession::Ownership::Borrowed, TransportKind::Serial);
    QSignalSpy warnings(&session, &TerminalSession::warningOccurred);
    QSignalSpy errors(&session, &TerminalSession::sessionError);
    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    QVERIFY(session.statistics().connectedAt.isValid());
    first.warningOccurred(QStringLiteral("RX capacity unverified"));
    QCOMPARE(warnings.count(), 1);
    QCOMPARE(warnings.last().at(1).toString(), QStringLiteral("RX capacity unverified"));
    QCOMPARE(session.state(), SessionState::Running);
    QCOMPARE(errors.count(), 0);

    first.simulateRemoteDisconnect();
    QTRY_COMPARE(session.state(), SessionState::Failed);
    QVERIFY(session.reconnect());
    QTRY_COMPARE(session.state(), SessionState::Running);
    first.warningOccurred(QStringLiteral("RX capacity still unverified"));
    QCOMPARE(warnings.count(), 2);
    QCOMPARE(errors.count(), 0);

    session.attach(&second, TerminalSession::Ownership::Borrowed, TransportKind::Serial);
    first.warningOccurred(QStringLiteral("old attachment warning"));
    QCOMPARE(warnings.count(), 2);
    second.warningOccurred(QStringLiteral("current attachment warning"));
    QCOMPARE(warnings.count(), 3);
    QCOMPARE(warnings.last().at(0).value<ITransport*>(), static_cast<ITransport*>(&second));
}

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

void SessionTests::serialLrzszSizeMatrix_data() { SerialLrzszTests::data(); }
void SessionTests::serialLrzszSizeMatrix() { SerialLrzszTests::run(); }

void SessionTests::realSerialSessionUsesExistingPumpForFileTransfer_data()
{
    QTest::addColumn<bool>("cancelTransfer");
    QTest::newRow("complete") << false;
    QTest::newRow("protocol-cancel-keeps-session") << true;
}

void SessionTests::realSerialSessionUsesExistingPumpForFileTransfer()
{
#ifdef Q_OS_LINUX
    QFETCH(bool, cancelTransfer);
    const int descriptor = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    QVERIFY(descriptor >= 0);
    QFile masterOwner;
    QVERIFY(masterOwner.open(descriptor, QIODevice::ReadWrite, QFileDevice::AutoCloseHandle));
    QVERIFY(::grantpt(descriptor) == 0);
    QVERIFY(::unlockpt(descriptor) == 0);
    const QString slave = QString::fromLocal8Bit(::ptsname(descriptor));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray payload(777, 'L');
    QFile source(directory.filePath(QStringLiteral("serial-source.bin")));
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(payload), qint64(payload.size()));
    source.close();
    TerminalCore core(80,24);
    RuntimeConfig config; config.transportKind = TransportKind::Serial;
    TerminalSession session(&core);
    SerialConfig serialConfig; serialConfig.portName = slave;
    session.attach(new SerialTransport(serialConfig), TerminalSession::Ownership::Adopt, TransportKind::Serial);
    QVERIFY(session.start());
    QTRY_VERIFY(session.state() == SessionState::Running);
    QVERIFY(session.canTransferFiles());
    auto* controller = session.serialFileTransfer();
    QVERIFY(controller);
    QSignalSpy finished(controller, &SerialFileTransferController::finished);
    QSignalSpy disconnected(&session, &TerminalSession::disconnected);
    const auto generation = session.statistics().generation;
    SerialTransferRequest transfer;
    transfer.protocol = SerialTransferProtocol::XmodemCrc;
    transfer.files = {source.fileName()};
    core.pasteText(QStringLiteral("old-command\r"));
    QVERIFY(controller->start(transfer));
    session.write(QByteArrayLiteral("SHOULD_NOT_REACH_DEVICE"));
    if (cancelTransfer) {
        QTRY_VERIFY(!controller->progress().preparing);
        controller->cancel();
    }
    namespace FT = NovaTerm::FileTransfer;
    FT::XmodemEngine peer;
    FT::TransferRequest peerRequest;
    peerRequest.expectedSize = static_cast<quint64>(payload.size());
    QVERIFY(peer.start(peerRequest, 0));
    QByteArray received, wire, peerInput, peerOutput;
    bool prompted = false;
    QElapsedTimer clock; clock.start();
    QSocketNotifier readable(descriptor, QSocketNotifier::Read);
    QTimer tick; tick.setInterval(1);
    const auto drive = [&] {
        char bytes[8192];
        const auto count = ::read(descriptor, bytes, sizeof(bytes));
        if (count > 0) { wire.append(bytes, count); peerInput.append(bytes, count); }
        for (int turns = 0; turns < 8; ++turns) {
            while (const auto action = peer.takeAction()) {
                if (action->kind == FT::ActionKind::WriteAt) {
                    QCOMPARE(action->offset, static_cast<quint64>(received.size()));
                    received.append(reinterpret_cast<const char*>(action->bytes.data()),
                                    static_cast<qsizetype>(action->bytes.size()));
                }
                QVERIFY(peer.completeOperation(action->id, {}, static_cast<quint64>(clock.elapsed())));
            }
            if (!peerInput.isEmpty()) {
                const auto consumed = peer.consume({peerInput.constData(),peerInput.size()},
                                                    static_cast<quint64>(clock.elapsed()));
                peerInput.remove(0, static_cast<qsizetype>(consumed.consumed));
            }
            const auto output = peer.pendingOutput();
            if (!output.empty()) {
                peerOutput.append(output.data, output.size);
                QVERIFY(peer.acknowledgeOutput(static_cast<std::size_t>(output.size),
                                              static_cast<quint64>(clock.elapsed())));
            }
            if (!prompted && peer.progress().state == FT::State::Closing) {
                peerOutput.append("_SERIAL_AFTER_TRANSFER"); prompted = true;
            }
            if (!peerOutput.isEmpty()) {
                const auto written = ::write(descriptor,peerOutput.constData(),
                                              static_cast<std::size_t>(peerOutput.size()));
                if (written > 0) peerOutput.remove(0,written);
            }
        }
        peer.advance(static_cast<quint64>(clock.elapsed()));
    };
    connect(&tick, &QTimer::timeout, &tick, drive);
    connect(&readable, &QSocketNotifier::activated, &tick, drive);
    tick.start();
    QTRY_VERIFY_WITH_TIMEOUT(!controller->isActive(), 10000);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.front().at(0).toBool(), !cancelTransfer);
    if (cancelTransfer) {
        QCOMPARE(session.state(), SessionState::Running);
        QCOMPARE(disconnected.size(), 0);
        QCOMPARE(session.statistics().generation, generation);
        QTRY_VERIFY(peer.progress().state == FT::State::Cancelled);
        QCOMPARE(received.size(), 0);
        const QByteArray prompt("_SERIAL_AFTER_TRANSFER");
        QCOMPARE(::write(descriptor, prompt.constData(), static_cast<std::size_t>(prompt.size())),
                 static_cast<ssize_t>(prompt.size()));
        session.write(QByteArrayLiteral("NORMAL_AFTER_CANCEL"));
        QTRY_VERIFY(wire.contains("NORMAL_AFTER_CANCEL"));
        QTRY_VERIFY(core.terminalState().viewport.at(0).text.find("_SERIAL_AFTER_TRANSFER") != std::string::npos);
    } else {
        QCOMPARE(received, payload);
    }
    tick.stop();
    QVERIFY(wire.contains("old-command"));
    QVERIFY(!wire.contains("SHOULD_NOT_REACH_DEVICE"));
    QVERIFY(core.waitForIdle());
    const auto state = core.terminalState();
    QVERIFY(std::any_of(state.viewport.begin(), state.viewport.end(), [](const auto& line) {
        return line.text.find("_SERIAL_AFTER_TRANSFER") != std::string::npos;
    }));
    QVERIFY(session.transport()->isConnected());
    QVERIFY(!session.inputArbiter()->hasTransferLease());
    session.close();
#else
    QSKIP("Linux raw PTY fixture; no real user serial device required");
#endif
}

void SessionTests::recordsOnceAcrossBackpressure()
{
    TerminalCore core(80, 24); FakeTransport transport; SessionInputPump pump(&transport, &core);
    QByteArray observed; quint64 omitted = 0;
    connect(&pump, &SessionInputPump::receiveObserved, this, [&](const QByteArray& b) { observed += b; });
    connect(&pump, &SessionInputPump::fileTransferOmitted, this, [&](quint64 n) { omitted += n; });
    pump.start();
    core.setScrollbackLimit(0);
    const QByteArray prefill(core.queueStatistics().capacity, 'p');
    QVERIFY(core.writeInput(prefill).acceptedBytes > 0);
    const QByteArray raw = QByteArray::fromHex("1b5b33316d410d0a00ffc328").repeated(50000);
    emit transport.readyRead(raw);
    QVERIFY(pump.statistics().pendingBytes > 0); // Exercise a real full Parser queue.
    pump.forwardFramerRemainder("replayed");
    emit core.inputBackpressureChanged(false);
    QTRY_COMPARE_WITH_TIMEOUT(pump.statistics().pendingBytes, qsizetype(0), 20000);
    QVERIFY(core.waitForIdle(20000)); QCoreApplication::processEvents();
    QCOMPARE(observed, raw);
    pump.setFileTransferConsumer([](const QByteArray&) { return true; });
    pump.setFileTransferMode(true); emit transport.readyRead(QByteArray(1024, '\xff'));
    QCOMPARE(omitted, quint64(1024)); QCOMPARE(observed, raw);
    pump.setFileTransferMode(false); emit transport.readyRead("after");
    QCOMPARE(observed, raw + "after");
}

void SessionTests::recordingLifecycleFollowsConnectionGeneration()
{
    TerminalCore core(80, 24); FakeTransport transport; TerminalSession session(&core);
    QSignalSpy received(&session, &TerminalSession::receiveObserved);
    QSignalSpy closed(&session, &TerminalSession::receiveStreamClosed);
    session.attach(&transport, TerminalSession::Ownership::Borrowed); QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    emit transport.readyRead("first"); QCOMPARE(received.size(), 1);
    QCOMPARE(received[0][0].toULongLong(), session.statistics().generation);
    const auto old = session.statistics().generation; transport.simulateRemoteDisconnect();
    QVERIFY(!closed.empty()); QCOMPARE(closed.back()[0].toULongLong(), old);
    QVERIFY(session.start()); QTRY_COMPARE(session.state(), SessionState::Running);
    emit transport.readyRead("second"); QCOMPARE(received.size(), 2);
    QCOMPARE(received[1][0].toULongLong(), session.statistics().generation);
    QVERIFY(session.statistics().generation > old);
}

void SessionTests::recordingBindingFollowsSessionIdentityReuse()
{
    using namespace NovaTerm::Recording;
    QTemporaryDir dir; HistoryService recording(dir.path()); TerminalCore core(80, 24);
    FakeTransport transport; TerminalSession session(&core); bindHistoryRecording(&session, &recording);
    const auto original = session.id(); session.close(); QVERIFY(session.resetForReuse()); QVERIFY(session.id() != original);
    session.attach(&transport, TerminalSession::Ownership::Borrowed); QVERIFY(session.start()); QTRY_COMPARE(session.state(), SessionState::Running);
    bool started = false; HistoryCaptureId capture;
    QVERIFY(recording.start({session.id(), session.statistics().generation}, [&](auto r) { QVERIFY(r.success); capture = r.captureId; started = true; })); QTRY_VERIFY(started);
    emit transport.readyRead("recorded after reuse"); transport.simulateRemoteDisconnect();
    QTRY_VERIFY_WITH_TIMEOUT(!recording.captures().front().active, 1000);
    bool read = false; QVERIFY(recording.readCommitted(capture, {}, [&](auto r) {
        QCOMPARE(r.integrity, Integrity::Lossless); QCOMPARE(r.records.size(), size_t(1));
        QCOMPARE(r.records.front().payload, QByteArray("recorded after reuse")); read = true;
    })); QTRY_VERIFY(read);
}

void SessionTests::fileTransferInputUsesSinglePumpAndIndependentPause()
{
    TerminalCore core(80,24);
    FakeTransport transport;
    QVERIFY(transport.connectToHost());
    SessionInputPump pump(&transport, &core);
    QByteArray protocol;
    pump.setFileTransferConsumer([&protocol](const QByteArray& bytes) {
        protocol.append(bytes); return true;
    });
    pump.setFileTransferMode(true);
    pump.start();
    QSignalSpy visible(&pump, &SessionInputPump::interactiveBytes);
    emit transport.readyRead(QByteArray("\0\x18\xff",3));
    QCOMPARE(protocol, QByteArray("\0\x18\xff",3));
    QCOMPARE(visible.count(), 0);
    pump.setFileTransferReadPaused(true);
    QVERIFY(transport.readPaused);
    emit core.inputBackpressureChanged(false);
    QVERIFY(transport.readPaused);
    pump.setFileTransferReadPaused(false);
    QVERIFY(!transport.readPaused);
    emit core.inputBackpressureChanged(true);
    QVERIFY(!transport.readPaused);
    pump.setFileTransferMode(false);
    QVERIFY(transport.readPaused);
    pump.setFileTransferConsumer({});
    emit core.inputBackpressureChanged(false);
    emit transport.readyRead(QByteArrayLiteral("terminal-text"));
    QCOMPARE(visible.count(), 1);
    QVERIFY(core.waitForIdle());
    QVERIFY(core.terminalState().viewport.at(0).text.find("terminal-text") != std::string::npos);
}

void SessionTests::fileTransferLeaseGatesAllInputAndResets()
{
    FakeTransport transport;
    QVERIFY(transport.connectToHost());
    SessionInputArbiter arbiter;
    arbiter.bind(&transport, 7);
    QByteArray fileBytes;
    const auto writer = [&fileBytes](QByteArrayView bytes) -> qint64 {
        const qsizetype accepted = qMin<qsizetype>(2, bytes.size());
        fileBytes.append(bytes.data(), accepted);
        return accepted;
    };
    QVERIFY(arbiter.acquireMcpLease(1, 7));
    QVERIFY(!arbiter.acquireTransferLease(2, 7, writer));
    arbiter.releaseMcpLease(1);
    QVERIFY(!arbiter.acquireTransferLease(2, 6, writer));
    QVERIFY(arbiter.acquireTransferLease(2, 7, writer));
    QVERIFY(!arbiter.acquireMcpLease(3, 7));
    arbiter.submitUserInput(QByteArrayLiteral("blocked"));
    QVERIFY(transport.writes.isEmpty());
    QVERIFY(arbiter.submitTerminalOutput(QByteArrayLiteral("old-command")));
    QCOMPARE(transport.writes, QByteArrayLiteral("old-command"));
    QVERIFY(arbiter.activateTransferLease(2));
    QVERIFY(!arbiter.submitTerminalOutput(QByteArrayLiteral("terminal-reply")));
    QCOMPARE(arbiter.submitTransferInput(2, QByteArrayView("ABC", 3)), qint64(2));
    QCOMPARE(fileBytes, QByteArrayLiteral("AB"));
    QCOMPARE(arbiter.submitTransferInput(4, QByteArrayView("X", 1)), qint64(-1));
    arbiter.releaseTransferLease(4);
    QVERIFY(arbiter.hasTransferLease());
    arbiter.reset(8);
    QVERIFY(!arbiter.hasTransferLease());
    arbiter.submitUserInput(QByteArrayLiteral("normal"));
    QCOMPARE(transport.writes, QByteArrayLiteral("old-commandnormal"));
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
    // 空规则不再禁用串口命令：降级为空闲收尾，仍不向设备探测提示符。
    const auto idle = ShellIntegration::profileFor(runtime);
    QVERIFY(idle);
    QVERIFY(idle->allowIdleCompletion);
    QVERIFY(!idle->allowUnverifiedPrompt);
    QVERIFY(!idle->shellIntegration);
    QVERIFY(idle->promptPattern.isEmpty());
    QVERIFY(!idle->requiresStartMarker);
    QCOMPARE(idle->lineEnding, QByteArrayLiteral("\r"));
    QCOMPARE(idle->idleQuietMs, 3000);

    runtime.transport.insert(QStringLiteral("interactivePromptPattern"),
                             QStringLiteral("device> $"));
    const auto profile = ShellIntegration::profileFor(runtime);
    QVERIFY(profile);
    QVERIFY(!profile->allowIdleCompletion);
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

namespace {
/** @brief 构造空闲收尾串口 Profile 的测试装置（静默窗缩短）。 */
struct IdleSerialHarness
{
    InteractiveCommandProfile profile;
    std::unique_ptr<TerminalCore> core;
    FakeTransport transport;
    SessionInputArbiter arbiter;
    InteractiveStreamFramer framer;
    std::unique_ptr<SessionCommandCoordinator> coordinator;
    std::unique_ptr<SessionInputPump> pump;

    explicit IdleSerialHarness(int idleQuietMs, bool remoteEcho = true)
    {
        RuntimeConfig runtime;
        runtime.transportKind = TransportKind::Serial;
        runtime.transport.insert(QStringLiteral("interactiveRemoteEcho"), remoteEcho);
        const auto configured = ShellIntegration::profileFor(runtime);
        if (!configured)
            qFatal("串口空闲收尾 Profile 缺失");
        profile = *configured;
        profile.idleQuietMs = idleQuietMs;
        core = std::make_unique<TerminalCore>(80, 24);
        if (!transport.connectToHost())
            qFatal("FakeTransport 连接失败");
        arbiter.bind(&transport, 60);
        framer.configure(profile);
        framer.reset(60);
        coordinator = std::make_unique<SessionCommandCoordinator>(&arbiter, &framer);
        coordinator->configure(profile);
        coordinator->reset(60);
        pump = std::make_unique<SessionInputPump>(&transport, core.get(), &framer);
        QObject::connect(pump.get(), &SessionInputPump::interactiveEvent,
                         coordinator.get(),
                         &SessionCommandCoordinator::handleInteractiveEvent);
        QObject::connect(pump.get(), &SessionInputPump::interactiveBytes,
                         coordinator.get(),
                         &SessionCommandCoordinator::handleInteractiveBytes);
        pump->start();
    }
};
} // namespace

void SessionTests::serialWithoutPromptCompletesByIdleQuiet()
{
    IdleSerialHarness harness(60);
    CommandExecutionRequest request;
    request.requestId = 501;
    request.command = QByteArrayLiteral("uname -srm");
    request.executionNonce = QByteArrayLiteral("idle-501");
    request.expectedPromptGeneration = 0;
    request.limits = {4096, 5000};
    QSignalSpy finished(harness.coordinator.get(),
                        &SessionCommandCoordinator::finished);
    QVERIFY(harness.coordinator->submit(request));
    QCOMPARE(harness.transport.writes, QByteArrayLiteral("uname -srm\r"));

    emit harness.transport.readyRead(
        QByteArrayLiteral("uname -srm\r\nLinux test\r\n"));
    QTRY_COMPARE(finished.count(), 1);
    const auto result = qvariant_cast<CommandExecutionResult>(finished.first().front());
    QCOMPARE(result.outcome, CommandExecutionOutcome::Completed);
    QVERIFY(result.terminationConfirmed);
    QCOMPARE(result.exitCode, std::optional<int>{});
    QCOMPARE(result.standardOutput, QByteArrayLiteral("Linux test\r\n"));
    QVERIFY(result.executionMayHaveStarted);
    // 回显仍照常上屏，只有捕获结果剥离首行。
    QVERIFY(harness.core->waitForIdle());
    QByteArray visible;
    for (const auto& line : harness.core->terminalState().viewport)
        visible += QByteArray::fromStdString(line.text);
    QVERIFY(visible.contains(QByteArrayLiteral("uname -srm")));
    QVERIFY(visible.contains(QByteArrayLiteral("Linux test")));
}

void SessionTests::serialIdleCompletionNoOutputFallsBackToDone()
{
    IdleSerialHarness harness(50);
    CommandExecutionRequest request;
    request.requestId = 502;
    request.command = QByteArrayLiteral("sync");
    request.executionNonce = QByteArrayLiteral("idle-502");
    request.expectedPromptGeneration = 0;
    request.limits = {4096, 5000};
    QSignalSpy finished(harness.coordinator.get(),
                        &SessionCommandCoordinator::finished);
    QVERIFY(harness.coordinator->submit(request));
    QCOMPARE(harness.transport.writes, QByteArrayLiteral("sync\r"));

    // 无任何设备输出：无响应兜底（min(4×50, 5000/2) = 200ms）收口，
    // 不会把已经跑完的静默命令拖到整体超时。
    QTRY_COMPARE(finished.count(), 1);
    const auto result = qvariant_cast<CommandExecutionResult>(finished.first().front());
    QCOMPARE(result.outcome, CommandExecutionOutcome::Completed);
    QVERIFY(result.terminationConfirmed);
    QCOMPARE(result.exitCode, std::optional<int>{});
    QVERIFY(result.standardOutput.isEmpty());
}

void SessionTests::serialIdleCompletionKeepsEchoWhenRemoteEchoDisabled()
{
    IdleSerialHarness harness(60, /*remoteEcho=*/false);
    QVERIFY(!harness.profile.remoteEcho);

    CommandExecutionRequest request;
    request.requestId = 503;
    request.command = QByteArrayLiteral("status");
    request.executionNonce = QByteArrayLiteral("idle-503");
    request.expectedPromptGeneration = 0;
    request.limits = {4096, 5000};
    QSignalSpy finished(harness.coordinator.get(),
                        &SessionCommandCoordinator::finished);
    QVERIFY(harness.coordinator->submit(request));
    QCOMPARE(harness.transport.writes, QByteArrayLiteral("status\r"));

    emit harness.transport.readyRead(QByteArrayLiteral("status\r\nok\r\n"));
    QTRY_COMPARE(finished.count(), 1);
    const auto result = qvariant_cast<CommandExecutionResult>(finished.first().front());
    QCOMPARE(result.outcome, CommandExecutionOutcome::Completed);
    // 远端未回显时不得把设备的第一行正常输出误当回显剥掉。
    QCOMPARE(result.standardOutput, QByteArrayLiteral("status\r\nok\r\n"));
}

void SessionTests::serialIdleCompletionTimeoutWinsOverLongQuiet()
{
    IdleSerialHarness harness(5000);
    CommandExecutionRequest request;
    request.requestId = 504;
    request.command = QByteArrayLiteral("tick");
    request.executionNonce = QByteArrayLiteral("idle-504");
    request.expectedPromptGeneration = 0;
    request.limits = {4096, 400};
    QSignalSpy finished(harness.coordinator.get(),
                        &SessionCommandCoordinator::finished);
    QVERIFY(harness.coordinator->submit(request));
    emit harness.transport.readyRead(QByteArrayLiteral("booting\r\n"));

    QTRY_COMPARE(finished.count(), 1);
    const auto result = qvariant_cast<CommandExecutionResult>(finished.first().front());
    QCOMPARE(result.outcome, CommandExecutionOutcome::TimedOut);
    QVERIFY(!result.terminationConfirmed);
    QCOMPARE(result.exitCode, std::optional<int>{});
}

void SessionTests::serialIdleCompletionPreemptedByUserInput()
{
    IdleSerialHarness harness(3000);
    CommandExecutionRequest request;
    request.requestId = 505;
    request.command = QByteArrayLiteral("sleep 1");
    request.executionNonce = QByteArrayLiteral("idle-505");
    request.expectedPromptGeneration = 0;
    request.limits = {4096, 5000};
    QSignalSpy finished(harness.coordinator.get(),
                        &SessionCommandCoordinator::finished);
    QVERIFY(harness.coordinator->submit(request));
    QCOMPARE(harness.transport.writes, QByteArrayLiteral("sleep 1\r"));

    harness.arbiter.submitUserInput(QByteArrayLiteral("x"));
    QCOMPARE(finished.count(), 1);
    const auto result = qvariant_cast<CommandExecutionResult>(finished.first().front());
    QCOMPARE(result.outcome, CommandExecutionOutcome::Cancelled);
    QVERIFY(result.executionMayHaveStarted);
    QVERIFY(!result.terminationConfirmed);
    QCOMPARE(harness.transport.writes, QByteArrayLiteral("sleep 1\rx"));
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

void SessionTests::serialCommandSettingsRejectInvalidInput()
{
    SerialConfig config;
    config.portName = QStringLiteral("test-not-opened");
    QVERIFY(config.isValid());
    config.interactivePromptPattern = QStringLiteral("[");
    QVERIFY(!config.isValid());
    config.interactivePromptPattern = QString(257, QLatin1Char('a'));
    QVERIFY(!config.isValid());
    config.interactivePromptPattern = QStringLiteral("^device> $");
    config.interactiveLineEnding = QStringLiteral("invalid");
    QVERIFY(!config.isValid());
    config.interactiveLineEnding = QStringLiteral("crlf");
    QVERIFY(config.isValid());
}

void SessionTests::attachmentSnapshotConfiguresDeviceCommands()
{
    TerminalCore core(80, 24);
    TerminalSession session(&core);
    FakeTransport transport;
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Serial;
    runtime.transport = {
        {QStringLiteral("portName"), QStringLiteral("test-not-opened")},
        {QStringLiteral("interactivePromptPattern"), QStringLiteral("^device> $")},
        {QStringLiteral("interactiveLineEnding"), QStringLiteral("crlf")},
        {QStringLiteral("interactiveRemoteEcho"), false}};
    session.attach(&transport, TerminalSession::Ownership::Borrowed, runtime);
    runtime.transport.insert(QStringLiteral("interactivePromptPattern"), QStringLiteral("changed"));
    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    QVERIFY(!session.commandCoordinator()->isPromptReady());
    emit transport.readyRead(QByteArrayLiteral("device> "));
    QTRY_VERIFY_WITH_TIMEOUT(session.commandCoordinator()->isPromptReady(), 500);
    QCOMPARE(transport.writes, QByteArray{});
    CommandExecutionRequest request;
    request.requestId = 102;
    request.command = QByteArrayLiteral("status");
    request.executionNonce = QByteArrayLiteral("serial-config-test");
    request.expectedPromptGeneration = session.commandCoordinator()->promptGeneration();
    request.limits = {1024, 1000};
    QVERIFY(session.commandCoordinator()->submit(request));
    QCOMPARE(transport.writes, QByteArrayLiteral("status\r\n"));
    QCOMPARE(session.runtimeConfig().transport.value(QStringLiteral("interactivePromptPattern")).toString(),
             QStringLiteral("^device> $"));
    QVERIFY(!session.runtimeConfig().transport.value(QStringLiteral("interactiveRemoteEcho")).toBool());
    session.close(CloseMode::Abort);
    QTRY_COMPARE(session.state(), SessionState::Closed);
    QVERIFY(session.resetForReuse());
    FakeTransport telnet;
    RuntimeConfig next;
    next.transportKind = TransportKind::Telnet;
    next.transport.insert(QStringLiteral("host"), QStringLiteral("other-host"));
    session.attach(&telnet, TerminalSession::Ownership::Borrowed, next);
    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    emit telnet.readyRead(QByteArrayLiteral("device> "));
    QTest::qWait(200);
    QVERIFY(!session.commandCoordinator()->isPromptReady());
    QVERIFY(!session.runtimeConfig().transport.contains(QStringLiteral("interactivePromptPattern")));
    QVERIFY(!session.runtimeConfig().transport.contains(QStringLiteral("portName")));
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
    QVERIFY(!session.runtimeConfig().transport.contains(QStringLiteral("interactivePromptPattern")));
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

void SessionTests::serialPendingReconnectReflectsActualState_data()
{
    QTest::addColumn<bool>("manual");
    QTest::addColumn<int>("seconds");
    QTest::addColumn<bool>("supportsReconnect");
    QTest::addColumn<bool>("reserved");
    QTest::newRow("enabled-manual-disconnect") << true << 60 << true << false;
    QTest::newRow("enabled-remote-disconnect") << false << 60 << true << true;
    QTest::newRow("disabled-remote-disconnect") << false << 0 << true << false;
    QTest::newRow("unsupported-remote-disconnect") << false << 60 << false << false;
}

void SessionTests::serialPendingReconnectReflectsActualState()
{
    QFETCH(bool, manual);
    QFETCH(int, seconds);
    QFETCH(bool, supportsReconnect);
    QFETCH(bool, reserved);
    RuntimeConfig config;
    config.transportKind = TransportKind::Serial;
    config.transport.insert(QStringLiteral("reconnectSeconds"), seconds);
    FakeTransport transport(supportsReconnect);
    TerminalSession session(config);
    session.attach(&transport, TerminalSession::Ownership::Borrowed);
    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    if (manual) QVERIFY(session.disconnectForReconnect());
    else transport.simulateRemoteDisconnect();
    QCOMPARE(session.state(), SessionState::Failed);
    QVERIFY(!transport.isConnected());
    QVERIFY(!transport.hasPendingDisconnect());
    QCOMPARE(session.runtimeConfig().transport.value("reconnectSeconds").toInt(), seconds);
    QCOMPARE(session.hasPendingAutomaticReconnect(), reserved);
    QCOMPARE(transport.connectAttempts, 1);
    session.close();
    QCOMPARE(session.state(), SessionState::Closed);
    QVERIFY(!session.hasPendingAutomaticReconnect());
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

void SessionTests::agentContextClearDropsPreviousSummary()
{
    TerminalCore core(20, 3);
    QVERIFY(core.writeInput("old-summary\r\nold-history\r\nthird\r\nfourth\r\n").fullyAccepted());
    QVERIFY(core.waitForIdle());
    TerminalContextProvider provider(&core);
    auto before = provider.trySnapshot(); QVERIFY(before); QVERIFY(!before->entries.empty());
    QVERIFY(core.clearAll()); QVERIFY(core.waitForIdle());
    auto after = provider.trySnapshot(); QVERIFY(after);
    QVERIFY(after->entries.empty());
    QVERIFY(after->resetRevision > before->state.revision);
    QVERIFY(core.writeInput("fresh-history\r\ntwo\r\nthree\r\nfour\r\n").fullyAccepted());
    QVERIFY(core.waitForIdle());
    after = provider.trySnapshot(); QVERIFY(after);
    QVERIFY(std::any_of(after->entries.begin(), after->entries.end(), [](const auto& entry) {
        return entry.text == "fresh-history";
    }));
}
void SessionTests::agentContextRejectsPreviousGenerationPublishedFallback()
{
    TerminalCore core(20, 3);
    QVERIFY(core.writeInput("obsolete-summary\r\nold\r\nold\r\nold\r\n").fullyAccepted());
    QVERIFY(core.waitForIdle());
    TerminalContextProvider provider(&core);
    QVERIFY(provider.trySnapshot());
    (void)core.requestPublishedTerminalState();
    QVERIFY(core.waitForIdle());
    const auto old = core.requestPublishedTerminalState(); QVERIFY(old);
    QVERIFY(core.clearAll()); QVERIFY(core.waitForIdle());
    const auto generation = core.cacheGeneration();
    QVERIFY(old->state.cacheGeneration != generation);
    const auto requests = core.publishedContextStatistics().requestCount;
    QVERIFY(core.writeInput(QByteArray(6 * 1024 * 1024, 'n')).fullyAccepted());
    for (int sample = 0; sample < 10000; ++sample) {
        const auto snapshot = provider.trySnapshot();
        if (snapshot) {
            QCOMPARE(snapshot->state.cacheGeneration, generation);
            QVERIFY(std::none_of(snapshot->entries.begin(), snapshot->entries.end(), [](const auto& entry) {
                return entry.text == "obsolete-summary";
            }));
        }
        if (core.publishedContextStatistics().requestCount > requests)
            break; // A busy model forced the real published-state fallback.
        std::this_thread::yield();
    }
    QVERIFY(core.publishedContextStatistics().requestCount > requests);
    QVERIFY(core.waitForIdle(15000));
    const auto snapshot = provider.trySnapshot(); QVERIFY(snapshot);
    QCOMPARE(snapshot->state.cacheGeneration, generation);
}
void SessionTests::agentContextJoinsHistorySeamAndBoundsUtf8()
{
    TerminalCore core(10, 3);
    const QByteArray wrapped("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN\r\n");
    QVERIFY(core.writeInput(wrapped).fullyAccepted());
    QVERIFY(core.waitForIdle());
    TerminalContextProvider provider(&core);
    const auto context = provider.trySnapshot(); QVERIFY(context);
    QVERIFY(std::any_of(context->entries.begin(), context->entries.end(), [](const auto& line) {
        return line.text == "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN";
    }));
    QVERIFY(core.writeInput(QByteArray("\x1b]2;") + QStringLiteral("中文").toUtf8() + '\x07').fullyAccepted());
    QVERIFY(core.waitForIdle());
    const auto bounded = core.tryTerminalState(0, 4, 1); QVERIFY(bounded);
    QCOMPARE(bounded->title, QStringLiteral("中").toStdString());
    QVERIFY(bounded->truncated);
    TerminalStateCache cache;
    for (NovaTerm::u64 revision = 1; revision <= 2048; ++revision)
        cache.append(revision, std::to_string(revision));
    QCOMPARE(cache.entries().size(), TerminalStateCache::MaxLines);
    QVERIFY(cache.floor() > 0);
}

void SessionTests::agentContextCaptureKeepsSuccessfulQtryResult_data()
{
    QTest::addColumn<bool>("bounded");
    QTest::newRow("provider-snapshot") << false;
    QTest::newRow("bounded-terminal-state") << true;
}
void SessionTests::agentContextCaptureKeepsSuccessfulQtryResult()
{
    QFETCH(bool, bounded);
    const auto exercise = [](auto makeCapture, auto revisionOf) {
        NovaTerm::u64 expected = 42;
        using Capture = decltype(makeCapture(expected));
        Capture capture;
        int reads = 0;
        const auto read = [&] { return ++reads == 1 ? makeCapture(expected) : Capture{}; };
        const auto matches = [&](const auto& value) { return revisionOf(value) == expected; };
        QTRY_VERIFY_WITH_TIMEOUT(sampleCurrentCapture(capture, read, matches), 50);
        QCOMPARE(reads, 1); QVERIFY(capture); QCOMPARE(revisionOf(*capture), expected);
        // A new input revision must not reuse the previous round's successful capture.
        expected = 43; reads = 0;
        QTRY_VERIFY_WITH_TIMEOUT(sampleCurrentCapture(capture, read, matches), 50);
        QCOMPARE(reads, 1); QVERIFY(capture); QCOMPARE(revisionOf(*capture), expected);
    };
    if (bounded) exercise([](NovaTerm::u64 revision) {
        NovaTerm::TerminalState state; state.revision = revision;
        return std::optional<NovaTerm::TerminalState>{std::move(state)};
    }, [](const auto& state) { return state.revision; });
    else exercise([](NovaTerm::u64 revision) {
        auto value = std::make_shared<TerminalContextProvider::Snapshot>(); value->state.revision = revision;
        return std::shared_ptr<const TerminalContextProvider::Snapshot>{std::move(value)};
    }, [](const auto& snapshot) { return snapshot.state.revision; });
}

void SessionTests::agentContextFiltersProgressWrapDuplicatesAndAlternate()
{
    TerminalCore core(10, 6);
    QVERIFY(core.waitForIdle());
    TerminalContextProvider provider(&core);
    std::shared_ptr<const TerminalContextProvider::Snapshot> context;
    NovaTerm::u64 expectedRevision = core.modelRevision();
    const auto feed = [&](const QByteArray& data) {
        context.reset();
        if (!core.writeInput(data).fullyAccepted() || !core.waitForIdle()) return false;
        expectedRevision = core.modelRevision();
        return true;
    };
    const auto contains = [](const auto& lines, const std::string& value) {
        return std::any_of(lines.begin(), lines.end(), [&](const auto& line) { return line.text == value; });
    };
    QVERIFY(feed("1234567890AB\r\n"));
    // Byte completion does not reserve the model lock. Wait for a current nonblocking capture.
    const auto captureCurrent = [&] {
        return sampleCurrentCapture(context, [&provider] { return provider.trySnapshot(); },
            [expectedRevision](const auto& snapshot) { return snapshot.state.revision == expectedRevision; });
    };
    QTRY_VERIFY_WITH_TIMEOUT(captureCurrent(), 1000);
    QVERIFY(contains(context->entries, "1234567890AB"));
    const auto retainedEntries = context->entries.size();
    for (int percent = 1; percent < 100; ++percent) {
        QVERIFY(feed("\r" + QByteArray::number(percent) + "%"));
        QTRY_VERIFY_WITH_TIMEOUT(captureCurrent(), 1000);
        QCOMPARE(context->entries.size(), retainedEntries);
    }
    const auto revision = context->state.revision;
    QVERIFY(feed("\r\x1b[2Kdone\r\ndone\r\n|\r\n"));
    QTRY_VERIFY_WITH_TIMEOUT(captureCurrent(), 1000);
    QCOMPARE(std::count_if(context->entries.begin(), context->entries.end(), [revision](const auto& line) {
        return line.revision > revision && line.text == "done";
    }), 1);
    QVERIFY(!contains(context->entries, "|"));
    const auto beforeAlternate = context->state.revision;
    QVERIFY(feed("\x1b[?1049h\x1b[Htop 1"));
    QTRY_VERIFY_WITH_TIMEOUT(captureCurrent(), 1000);
    QVERIFY(context->state.alternateScreen); QVERIFY(context->entries.empty());
    QVERIFY(context->resetRevision > beforeAlternate);
    for (int frame = 0; frame < 100; ++frame) {
        QVERIFY(feed("\x1b[Htop " + QByteArray::number(frame)));
        QTRY_VERIFY_WITH_TIMEOUT(captureCurrent(), 1000);
        QVERIFY(context->entries.empty()); QVERIFY(context->state.viewport.size() <= 6);
    }
    QVERIFY(feed("\x1b[?1049l\x1b]2;agent-title\x07"));
    QTRY_VERIFY_WITH_TIMEOUT(captureCurrent(), 1000);
    QVERIFY(!context->state.alternateScreen); QCOMPARE(context->state.title, std::string("agent-title"));
    auto bounded = core.tryTerminalState(0, 12, 2);
    QTRY_VERIFY_WITH_TIMEOUT(sampleCurrentCapture(bounded, [&core] { return core.tryTerminalState(0, 12, 2); },
        [expectedRevision](const auto& state) { return state.revision == expectedRevision; }), 1000);
    std::size_t bytes = bounded->title.size();
    for (const auto& line : bounded->viewport) bytes += line.text.size();
    for (const auto& line : bounded->recentOutput) bytes += line.text.size();
    QVERIFY(bytes <= 12);
    QVERIFY(bounded->viewport.size() + bounded->recentOutput.size() <= 2);
    QVERIFY(bounded->truncated);
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

void SessionTests::reconnectResetsTerminalParser_data()
{
    QTest::addColumn<QByteArray>("residue");
    QTest::newRow("graphics-charset") << QByteArray("\x1b(0");
    QTest::newRow("unfinished-osc") << QByteArray("\x1b]2;unfinished");
    QTest::newRow("unfinished-csi") << QByteArray("\x1b[12;");
    QTest::newRow("unfinished-utf8") << QByteArray::fromHex("e4b8");
    QTest::newRow("alternate-screen") << QByteArray("\x1b[?1049h\x1b[?1003h");
}

void SessionTests::reconnectResetsTerminalParser()
{
    QFETCH(QByteArray, residue);
    RuntimeConfig config;
    config.transportKind = TransportKind::Ssh;
    TerminalSession session(config);
    auto* transport = new FakeTransport;
    session.attach(transport, TerminalSession::Ownership::Adopt, TransportKind::Ssh);
    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    emit transport->readyRead(residue);
    QVERIFY(session.core()->waitForIdle());
    transport->simulateRemoteDisconnect();
    QVERIFY(session.reconnect());
    QTRY_COMPARE(session.state(), SessionState::Running);
    emit transport->readyRead(QByteArray("lqqk ASCII ") + QByteArray::fromHex("e4b8ade69687"));
    QVERIFY(session.core()->waitForIdle());
    QVERIFY(!session.core()->isAlternateScreen());
    const auto state = session.core()->terminalState();
    QCOMPARE(QByteArray::fromStdString(state.viewport.at(0).text),
             QByteArray("lqqk ASCII ") + QByteArray::fromHex("e4b8ade69687"));
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

void SessionTests::terminalProtocolReplyDoesNotPreemptMcpLease()
{
    TerminalCore core(80, 24);
    FakeTransport transport;
    TerminalSession session(&core);
    session.attach(&transport, TerminalSession::Ownership::Borrowed);
    QVERIFY(session.start());
    auto* arbiter = session.inputArbiter();
    QVERIFY(arbiter->acquireMcpLease(91, session.statistics().generation));
    const auto generation = arbiter->userInputGeneration();
    core.writeInput(QByteArrayLiteral("\x1b[6n"));
    QVERIFY(core.waitForIdle(1000));
    QCoreApplication::processEvents();
    QVERIFY(arbiter->hasMcpLease());
    QCOMPARE(arbiter->userInputGeneration(), generation);
}

void SessionTests::newInputInvalidatesLegacyCommandReadiness()
{
    RuntimeConfig config;
    config.transportKind = TransportKind::LocalShell;
    config.transport.insert("interactiveShellKind", "posix");
    TerminalSession session(config); FakeTransport transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed, config);
    QVERIFY(session.start()); QTRY_COMPARE(session.state(), SessionState::Running);
    auto* controller = session.terminalController();
    emit transport.readyRead(QByteArrayLiteral("\x1b]633;NT;PROMPT;1;0\a"));
    QTRY_VERIFY(controller->readyForCommand());
    QVERIFY(session.commandCoordinator()->isPromptReady());
    QVERIFY(controller->acquire("client"));
    NovaTerm::TerminalInputAction action; action.text = "echo unsafe;";
    QVERIFY(controller->submit("client", "operation", {action}, false));
    QTRY_COMPARE(transport.writes, QByteArray("echo unsafe;"));
    controller->release("client");
    QVERIFY(!session.commandCoordinator()->isPromptReady());
    QVERIFY(!controller->readyForCommand());
}

void SessionTests::emptyAgentBatchPreservesCommandReadiness()
{
    RuntimeConfig config;
    config.transportKind = TransportKind::LocalShell;
    config.transport.insert("interactiveShellKind", "posix");
    TerminalSession session(config); FakeTransport transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed, config);
    QVERIFY(session.start()); QTRY_COMPARE(session.state(), SessionState::Running);
    auto* controller = session.terminalController();
    emit transport.readyRead(QByteArrayLiteral("\x1b]633;NT;PROMPT;1;0\a"));
    QTRY_VERIFY(controller->readyForCommand());
    QVERIFY(controller->acquire("client"));
    NovaTerm::TerminalInputAction action;
    QVERIFY(controller->submit("client", "operation", {action}, false));
    QTRY_VERIFY(!controller->pending());
    QVERIFY(transport.writes.isEmpty());
    QVERIFY(session.commandCoordinator()->isPromptReady());
    QVERIFY(controller->readyForCommand());
}

void SessionTests::shellIntegrationIsIndependentOfTransport_data()
{
    QTest::addColumn<int>("kind");
    QTest::newRow("local") << int(TransportKind::LocalShell);
    QTest::newRow("ssh") << int(TransportKind::Ssh);
    QTest::newRow("serial") << int(TransportKind::Serial);
    QTest::newRow("telnet") << int(TransportKind::Telnet);
    QTest::newRow("custom") << int(TransportKind::Custom);
}

void SessionTests::shellIntegrationIsIndependentOfTransport()
{
    QFETCH(int, kind);
    RuntimeConfig config;
    config.transportKind = static_cast<TransportKind>(kind);
    config.transport.insert("interactiveShellKind", "posix");
    TerminalSession session(config); FakeTransport transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed, config);
    QVERIFY(session.start());
    auto* controller = session.terminalController();
    QVERIFY(controller->integrated());
    QVERIFY(transport.writes.isEmpty());
    emit transport.readyRead(QByteArrayLiteral("\x1b]633;NT;PROMPT;1;0\a"));
    QTRY_VERIFY(controller->readyForCommand());
    QVERIFY(controller->acquire("client"));
    NovaTerm::TerminalInputAction text; text.text = "pwd";
    NovaTerm::TerminalInputAction enter;
    enter.kind = NovaTerm::TerminalInputAction::Kind::Bytes; enter.text = "\r";
    QVERIFY(controller->submit("client", "command", {text, enter}, true));
    QTRY_COMPARE(transport.writes, QByteArray("pwd\r"));
    emit transport.readyRead(QByteArrayLiteral("/tmp\r\n\x1b]633;NT;PROMPT;2;0\a"));
    QTRY_VERIFY(!controller->commandActive());
    const auto command = controller->observation("client").value("command").toObject();
    QCOMPARE(command.value("state").toString(), QString("finished"));
    QCOMPARE(command.value("exitCode").toInt(-1), 0);
}

void SessionTests::freeTerminalInputTracksOnlyIntegratedEvidence()
{
    RuntimeConfig config; config.transportKind = TransportKind::Serial;
    config.transport.insert("interactiveShellKind", "posix");
    TerminalSession session(config); FakeTransport transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed, config);
    QVERIFY(session.start());
    auto* controller = session.terminalController();
    emit transport.readyRead(QByteArrayLiteral("\x1b]633;NT;PROMPT;1;0\a"));
    QTRY_VERIFY(controller->readyForCommand());
    QVERIFY(controller->acquire("client"));
    NovaTerm::TerminalInputAction text; text.text = "false";
    NovaTerm::TerminalInputAction enter;
    enter.kind = NovaTerm::TerminalInputAction::Kind::Key; enter.key = NovaTerm::InputKey::Enter;
    QVERIFY(controller->submit("client", "free-input", {text, enter}, false));
    QTRY_COMPARE(transport.writes, QByteArray("false\r"));
    // Input newline alone must not invent a command boundary or exit status.
    QCOMPARE(controller->observation("client").value("command").toObject()
        .value("state").toString(), QString("none"));
    emit transport.readyRead(QByteArrayLiteral("\x1b]633;NT;PROMPT;2;1\a"));
    QTRY_COMPARE(controller->observation("client").value("command").toObject()
        .value("state").toString(), QString("finished"));
    const auto observation = controller->observation("client");
    const auto command = observation.value("command").toObject();
    QVERIFY(!command.value("executionId").toString().isEmpty());
    QCOMPARE(command.value("exitCode").toInt(-1), 1);
    QCOMPARE(command.value("result").toString(), QString("failure"));
    QCOMPARE(observation.value("lastCommand").toObject().value("executionId"), command.value("executionId"));
    QVERIFY(controller->submit("client", "typing-next", {text}, false));
    QTRY_VERIFY(!controller->pending());
    QCOMPARE(controller->observation("client").value("command").toObject()
        .value("state").toString(), QString("none"));
    QCOMPARE(controller->observation("client").value("lastCommand").toObject()
        .value("executionId"), command.value("executionId"));
}

void SessionTests::cacheInvalidationRejectsDeferredAgentInput_data()
{
    QTest::addColumn<int>("mutation");
    QTest::addColumn<int>("inputKind");
    for (int mutation = 0; mutation < 3; ++mutation)
        for (int kind = 0; kind < 3; ++kind)
            QTest::newRow(qPrintable(QString("mutation-%1-input-%2").arg(mutation).arg(kind)))
                << mutation << kind;
}

void SessionTests::cacheInvalidationRejectsDeferredAgentInput()
{
    QFETCH(int, mutation);
    QFETCH(int, inputKind);
    TerminalCore core(80, 3);
    QVERIFY(core.writeInput("old\r\nold\r\nold\r\nold\r\nold\r\n").fullyAccepted());
    QVERIFY(core.waitForIdle());
    FakeTransport transport;
    QVERIFY(transport.connectToHost());
    SessionInputArbiter arbiter;
    arbiter.bind(&transport, 1);
    InteractiveStreamFramer framer;
    InteractiveCommandProfile profile;
    profile.shellIntegration = true;
    framer.configure(profile);
    framer.reset(1);
    SessionCommandCoordinator coordinator(&arbiter, &framer);
    SessionTerminalController controller(&core, &arbiter, &coordinator, &framer,
        1, true, "\r", nullptr);
    QVERIFY(controller.acquire("client"));
    controller.handleEvent({InteractiveStreamEventKind::PromptReady, 1, std::nullopt});
    NovaTerm::TerminalInputAction action;
    action.text = "deferred";
    if (inputKind == 2) {
        QVERIFY(controller.submit("client", "running", {action}, true));
        QTRY_VERIFY(!controller.pending());
        QVERIFY(controller.commandActive());
        transport.writes.clear();
    }
    const auto running = controller.observation("client").value("command").toObject();
    const auto generation = core.cacheGeneration();
    QVERIFY(controller.submit("client", "deferred", {action}, inputKind == 1));
    // Let Parser finish encoding and invalidate the cache, while withholding all
    // GUI event delivery. Its queued encoding callback must recheck generation.
    if (mutation == 0) QVERIFY(core.clearAll());
    else if (mutation == 1) QVERIFY(core.writeInput("\x1b[3J").fullyAccepted());
    else core.resize(80, 12);
    QVERIFY(core.waitForIdle());
    QVERIFY(core.cacheGeneration() != generation);
    QVERIFY(controller.pending());
    QVERIFY(transport.writes.isEmpty());
    QTRY_VERIFY(!controller.pending());
    QVERIFY(transport.writes.isEmpty());
    QCOMPARE(controller.operation().value("status").toString(), QString("failed"));
    QVERIFY(!controller.operation().value("inputMayHaveBeenSent").toBool());
    QVERIFY(!controller.readyForCommand());
    if (inputKind == 2) {
        QVERIFY(controller.commandActive());
        QCOMPARE(controller.observation("client").value("command").toObject(), running);
    } else QVERIFY(!controller.commandActive());
}

void SessionTests::deferredInputAdmissionRejectsOldCacheGeneration()
{
    TerminalCore core(80, 3);
    FakeTransport transport;
    QVERIFY(transport.connectToHost());
    SessionInputArbiter arbiter;
    arbiter.bind(&transport, 1);
    InteractiveStreamFramer framer;
    SessionCommandCoordinator coordinator(&arbiter, &framer);
    SessionTerminalController controller(&core, &arbiter, &coordinator, &framer,
        1, false, "\r", nullptr);
    QVERIFY(controller.acquire("client"));
    const auto generation = core.cacheGeneration();
    QVERIFY(core.clearAll());
    QVERIFY(core.waitForIdle());
    QVERIFY(core.cacheGeneration() != generation);
    NovaTerm::TerminalInputAction action;
    action.text = "stale";
    QVERIFY(!controller.submit("client", "old-observation", {action}, false, -1, generation));
    QVERIFY(!controller.pending());
    QVERIFY(core.waitForIdle());
    QCoreApplication::processEvents();
    QVERIFY(transport.writes.isEmpty());
    QCOMPARE(controller.operation().value("status").toString(), QString("none"));
    QVERIFY(controller.submit("client", "new-observation", {action}, false, -1, core.cacheGeneration()));
    QTRY_VERIFY(!controller.pending());
    QCOMPARE(transport.writes, QByteArray("stale"));
}

void SessionTests::cacheClearCanOverlapAnAdmittedTransportWrite()
{
    TerminalCore core(80, 3);
    QVERIFY(core.waitForIdle());
    class OverlapTransport final : public ITransport {
    public:
        bool connectToHost() override { return true; }
        void disconnect() override {}
        void write(const QByteArray& data) override {
            if (onWrite) onWrite();
            writes += data;
            timeline.append("write-return");
        }
        void resizeTerminal(int, int) override {}
        bool isConnected() const override { return true; }
        QString errorString() const override { return {}; }
        std::function<void()> onWrite;
        QByteArray writes;
        QStringList timeline;
    } transport;
    SessionInputArbiter arbiter; arbiter.bind(&transport, 1);
    InteractiveStreamFramer framer;
    SessionCommandCoordinator coordinator(&arbiter, &framer);
    SessionTerminalController controller(&core, &arbiter, &coordinator, &framer,
        1, false, "\r", nullptr);
    QVERIFY(controller.acquire("client"));
    const auto generation = core.cacheGeneration();
    connect(&core, &TerminalCore::clearCompleted, &controller,
        [&transport](quint64 operation, quint64, int status, quint64) {
            QCOMPARE(operation, quint64(777)); QCOMPARE(status, 0);
            transport.timeline.append("clear-notified");
        });
    transport.onWrite = [&] {
        transport.timeline.append("write-enter");
        QVERIFY(!controller.pending());
        QCOMPARE(controller.operation().value("status").toString(), QString("submitted"));
        // A transport may queue work. Once the GUI has admitted this write,
        // Parser can clear independently while the same GUI callback is still
        // running; only the GUI completion notification must remain ordered.
        QVERIFY(core.requestClear(777, true, generation, {}));
        QVERIFY(core.waitForIdle());
        QVERIFY(core.cacheGeneration() != generation);
        QVERIFY(transport.writes.isEmpty());
        QVERIFY(!transport.timeline.contains("clear-notified"));
        transport.timeline.append("parser-cleared");
    };
    NovaTerm::TerminalInputAction action; action.text = "admitted";
    QVERIFY(controller.submit("client", "accepted-before-clear", {action}, false, -1, generation));
    QTRY_COMPARE(transport.writes, QByteArray("admitted"));
    QTRY_VERIFY(transport.timeline.contains("clear-notified"));
    QCOMPARE(transport.timeline, (QStringList{"write-enter", "parser-cleared", "write-return", "clear-notified"}));
    QVERIFY(controller.operation().value("inputMayHaveBeenSent").toBool());
}

void SessionTests::recentCommandExpiresWithoutDiscardingActiveCommand()
{
    TerminalCore core(80, 24); FakeTransport transport; QVERIFY(transport.connectToHost());
    SessionInputArbiter arbiter; arbiter.bind(&transport, 1);
    InteractiveStreamFramer framer;
    InteractiveCommandProfile profile; profile.shellIntegration = true;
    framer.configure(profile); framer.reset(1);
    SessionCommandCoordinator coordinator(&arbiter, &framer);
    SessionTerminalController controller(&core, &arbiter, &coordinator, &framer,
        1, true, "\r", nullptr);
    QVERIFY(controller.acquire("client"));
    controller.handleEvent({InteractiveStreamEventKind::PromptReady, 1, std::nullopt});
    NovaTerm::TerminalInputAction text; text.text = "pwd";
    QVERIFY(controller.submit("client", "first", {text}, true));
    QTRY_VERIFY(!controller.pending());
    controller.handleEvent({InteractiveStreamEventKind::CommandFinishedAtPrompt, 2, 0});
    const auto first = controller.observation("client").value("command").toObject();
    QVERIFY(controller.submit("client", "second", {text}, true));
    QTRY_VERIFY(!controller.pending());
    const auto secondId = controller.executionId();
    QCOMPARE(controller.observation("client").value("lastCommand").toObject()
        .value("executionId"), first.value("executionId"));
    // Exercise the real expiration handler with a short interval, without adding a
    // production test-only clock/expiration method or waiting ten minutes.
    auto* retention = controller.findChild<QTimer*>("commandRecordRetention");
    QVERIFY(retention);
    retention->start(20);
    QTRY_VERIFY(controller.observation("client").value("lastCommand").toObject().isEmpty());
    QVERIFY(controller.commandActive());
    QCOMPARE(controller.executionId(), secondId);
}

void SessionTests::freeInputKeepsTrailingTextOutOfCommandReadiness_data()
{
    QTest::addColumn<QByteArray>("paste");
    QTest::newRow("enter-then-text") << QByteArray{};
    QTest::newRow("blank-line-and-tail") << QByteArray("\nrm ");
    QTest::newRow("command-and-tail") << QByteArray("true\nrm ");
    QTest::newRow("two-submitted-lines") << QByteArray("true\nfalse\n");
}

void SessionTests::freeInputKeepsTrailingTextOutOfCommandReadiness()
{
    QFETCH(QByteArray, paste);
    RuntimeConfig config; config.transportKind = TransportKind::Serial;
    config.transport.insert("interactiveShellKind", "posix");
    TerminalSession session(config); FakeTransport transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed, config);
    QVERIFY(session.start());
    auto* controller = session.terminalController();
    emit transport.readyRead(QByteArrayLiteral("\x1b]633;NT;PROMPT;1;0\a"));
    QTRY_VERIFY(controller->readyForCommand());
    QVERIFY(controller->acquire("client"));
    std::vector<NovaTerm::TerminalInputAction> actions;
    if (paste.isEmpty()) {
        NovaTerm::TerminalInputAction enter;
        enter.kind = NovaTerm::TerminalInputAction::Kind::Key; enter.key = NovaTerm::InputKey::Enter;
        NovaTerm::TerminalInputAction tail; tail.text = "rm "; actions = {enter, tail};
    } else {
        NovaTerm::TerminalInputAction action; action.kind = NovaTerm::TerminalInputAction::Kind::Paste;
        action.text = paste.toStdString(); actions = {action};
    }
    QVERIFY(controller->submit("client", "free", actions, false));
    QTRY_VERIFY(!controller->pending());
    const auto written = transport.writes;
    emit transport.readyRead(QByteArrayLiteral("\x1b]633;NT;PROMPT;2;0\a"));
    QTest::qWait(30);
    QVERIFY(!controller->readyForCommand());
    QCOMPARE(controller->observation("client").value("command").toObject()
        .value("state").toString(), QString("none"));
    NovaTerm::TerminalInputAction next; next.text = "x";
    QVERIFY(!controller->submit("client", "must-not-append", {next}, true));
    QCOMPARE(transport.writes, written);
    if (paste == "true\nfalse\n") {
        emit transport.readyRead(QByteArrayLiteral("\x1b]633;NT;PROMPT;3;1\a"));
        QTRY_VERIFY(controller->readyForCommand());
    }
}

void SessionTests::nonemptyAgentBatchInvalidatesPreWritePrompt_data()
{
    QTest::addColumn<bool>("device");
    QTest::addColumn<bool>("candidate");
    QTest::newRow("integrated-ready") << false << false;
    QTest::newRow("device-ready") << true << false;
    QTest::newRow("device-candidate") << true << true;
}

void SessionTests::nonemptyAgentBatchInvalidatesPreWritePrompt()
{
    QFETCH(bool, device); QFETCH(bool, candidate);
    RuntimeConfig config; config.transportKind = TransportKind::Serial;
    if (device) config.transport.insert("interactivePromptPattern", "^device> $");
    else config.transport.insert("interactiveShellKind", "posix");
    TerminalSession session(config); FakeTransport transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed, config);
    QVERIFY(session.start());
    auto* controller = session.terminalController();
    QVERIFY(controller->acquire("client"));
    NovaTerm::TerminalInputAction text; text.text = "rm ";
    QVERIFY(controller->submit("client", "partial", {text}, false));
    QVERIFY(session.core()->waitForIdle(1000));
    controller->handleEvent({candidate ? InteractiveStreamEventKind::PromptCandidate
        : InteractiveStreamEventKind::PromptReady, 2, std::nullopt});
    QTRY_COMPARE(transport.writes, QByteArray("rm "));
    QTest::qWait(200);
    QVERIFY(!controller->readyForCommand());
}

void SessionTests::queuedUserInputPreemptsLaterAgentBatch()
{
    TerminalCore core(80, 24);
    FakeTransport transport;
    TerminalSession session(&core);
    session.attach(&transport, TerminalSession::Ownership::Borrowed);
    QVERIFY(session.start()); QTRY_COMPARE(session.state(), SessionState::Running);
    auto* controller = session.terminalController();
    QVERIFY(controller->acquire("client"));
    QVERIFY(core.writeInput(QByteArray(6 * 1024 * 1024, 'x')).fullyAccepted());
    core.processTextInput(QStringLiteral("u"));
    NovaTerm::TerminalInputAction action; action.text = "a";
    QVERIFY(controller->submit("client", "operation", {action}, false));
    QVERIFY(core.waitForIdle(15'000));
    QTRY_COMPARE(transport.writes, QByteArray("u"));
    QVERIFY(controller->owner().isEmpty());
}

void SessionTests::emptyAgentBatchKeepsLatestPromptEvidence_data()
{
    QTest::addColumn<bool>("device");
    QTest::addColumn<bool>("initialReady");
    QTest::addColumn<QByteArray>("incoming");
    QTest::addColumn<bool>("framingError");
    QTest::addColumn<quint64>("expectedPrompt");
    QTest::newRow("shell-reset") << false << true
        << QByteArray("\x1b]633;NT;RESET\a") << false << quint64(0);
    QTest::newRow("framing-error") << false << true
        << QByteArray{} << true << quint64(0);
    QTest::newRow("device-output") << true << true
        << QByteArray("Password: ") << false << quint64(0);
    QTest::newRow("new-shell-prompt") << false << true
        << QByteArray("\x1b]633;NT;PROMPT;2;0\a") << false << quint64(2);
    QTest::newRow("first-shell-prompt") << false << false
        << QByteArray("\x1b]633;NT;PROMPT;2;0\a") << false << quint64(2);
}

void SessionTests::emptyAgentBatchKeepsLatestPromptEvidence()
{
    QFETCH(bool, device);
    QFETCH(bool, initialReady);
    QFETCH(QByteArray, incoming);
    QFETCH(bool, framingError);
    QFETCH(quint64, expectedPrompt);
    RuntimeConfig config;
    config.transportKind = device ? TransportKind::Serial : TransportKind::LocalShell;
    if (device) config.transport.insert("interactivePromptPattern", "^device> $");
    else config.transport.insert("interactiveShellKind", "posix");
    TerminalSession session(config); FakeTransport transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed, config);
    QVERIFY(session.start()); QTRY_COMPARE(session.state(), SessionState::Running);
    auto* controller = session.terminalController();
    if (initialReady) {
        emit transport.readyRead(device ? QByteArray("device> ")
            : QByteArray("\x1b]633;NT;PROMPT;1;0\a"));
        QTRY_VERIFY(controller->readyForCommand());
    }
    QVERIFY(controller->acquire("client"));
    NovaTerm::TerminalInputAction action;
    QVERIFY(controller->submit("client", "operation", {action}, false));
    // 编码结果仅排队至 GUI；在处理回调前注入真实入站事件，固定事件顺序。
    QVERIFY(session.core()->waitForIdle(1000));
    if (!incoming.isEmpty()) emit transport.readyRead(incoming);
    if (framingError)
        controller->handleEvent({InteractiveStreamEventKind::FramingError, 0, std::nullopt});
    QTRY_VERIFY(!controller->pending());
    QVERIFY(transport.writes.isEmpty());
    QCOMPARE(controller->promptGeneration(), expectedPrompt);
    QCOMPARE(controller->readyForCommand(), expectedPrompt != 0);
}

void SessionTests::integratedCtrlCRecoversReadinessAtNewPrompt_data()
{
    QTest::addColumn<int>("kind");
    QTest::addColumn<bool>("typedFirst");
    QTest::newRow("ssh-idle") << int(TransportKind::Ssh) << false;
    QTest::newRow("ssh-unsubmitted-text") << int(TransportKind::Ssh) << true;
    QTest::newRow("serial-idle") << int(TransportKind::Serial) << false;
    QTest::newRow("serial-unsubmitted-text") << int(TransportKind::Serial) << true;
}

void SessionTests::integratedCtrlCRecoversReadinessAtNewPrompt()
{
    QFETCH(int, kind);
    QFETCH(bool, typedFirst);
    RuntimeConfig config;
    config.transportKind = static_cast<TransportKind>(kind);
    config.transport.insert("interactiveShellKind", "posix");
    TerminalSession session(config);
    FakeTransport transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed, config);
    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    auto* controller = session.terminalController();
    emit transport.readyRead(QByteArrayLiteral("\x1b]633;NT;PROMPT;1;0\a"));
    QTRY_VERIFY(controller->readyForCommand());
    QVERIFY(controller->acquire("client"));

    using A = NovaTerm::TerminalInputAction;
    std::vector<A> actions;
    if (typedFirst) actions.push_back({A::Kind::Text, "unfinished"});
    actions.push_back({A::Kind::Key, "c", NovaTerm::InputKey::None, 4});
    QVERIFY(controller->submit("client", "interrupt-idle-input", actions, false));
    const QByteArray interrupted = (typedFirst ? QByteArray("unfinished") : QByteArray{}) + '\x03';
    QTRY_COMPARE(transport.writes, interrupted);
    QTRY_VERIFY(!controller->pending());
    QVERIFY(!controller->readyForCommand());
    QVERIFY(!controller->commandActive());

    // 真实的新集成提示符才恢复就绪；发送中断本身不虚构命令完成。
    emit transport.readyRead(QByteArrayLiteral("\x1b]633;NT;PROMPT;2;130\a"));
    QTRY_VERIFY(controller->readyForCommand());
    QCOMPARE(controller->promptGeneration(), quint64(2));
    QCOMPARE(controller->observation("client").value("command").toObject()
        .value("state").toString(), QString("none"));
    QVERIFY(!controller->commandActive());

    QVERIFY(controller->submit("client", "next-command",
        {{A::Kind::Text, "pwd"}, {A::Kind::Bytes, "\r"}}, true));
    QTRY_COMPARE(transport.writes, interrupted + "pwd\r");
}

void SessionTests::terminalRawInputPreservesConfiguredEndingAndKeys_data()
{
    QTest::addColumn<int>("kind");
    QTest::addColumn<QString>("endingName");
    QTest::addColumn<QByteArray>("endingBytes");
    QTest::addColumn<bool>("applicationCursor");
    for (const auto kind : {TransportKind::Ssh, TransportKind::Serial}) {
        const auto transport = kind == TransportKind::Ssh ? QStringLiteral("ssh") : QStringLiteral("serial");
        for (const auto& ending : {QStringLiteral("cr"), QStringLiteral("lf"), QStringLiteral("crlf")}) {
            const auto bytes = ending == "lf" ? QByteArray("\n")
                : ending == "crlf" ? QByteArray("\r\n") : QByteArray("\r");
            for (const bool application : {false, true}) {
                QTest::newRow(qPrintable(transport + '-' + ending
                    + (application ? "-application-cursor" : "-normal-cursor")))
                    << int(kind) << ending << bytes << application;
            }
        }
    }
}

void SessionTests::terminalRawInputPreservesConfiguredEndingAndKeys()
{
    QFETCH(int, kind);
    QFETCH(QString, endingName);
    QFETCH(QByteArray, endingBytes);
    QFETCH(bool, applicationCursor);
    RuntimeConfig config;
    config.transportKind = static_cast<TransportKind>(kind);
    config.transport.insert("interactiveLineEnding", endingName);
    TerminalSession session(config);
    FakeTransport transport;
    session.attach(&transport, TerminalSession::Ownership::Borrowed, config);
    QVERIFY(session.start());
    QTRY_COMPARE(session.state(), SessionState::Running);
    auto* controller = session.terminalController();
    QVERIFY(!controller->readyForCommand());
    QCOMPARE(controller->lineEnding(), endingBytes);
    QVERIFY(transport.writes.isEmpty());
    if (applicationCursor) {
        emit transport.readyRead(QByteArrayLiteral("\x1b[?1h"));
        QVERIFY(session.core()->waitForIdle());
    }
    QVERIFY(controller->acquire("client"));

    // 原始输入不要求 Shell 提示符；方向键遵从终端当前游标模式。
    using A = NovaTerm::TerminalInputAction;
    const std::vector<A> actions{
        {A::Kind::Text, "raw"},
        {A::Kind::Key, "c", NovaTerm::InputKey::None, 4},
        {A::Kind::Key, {}, NovaTerm::InputKey::Tab},
        {A::Kind::Key, {}, NovaTerm::InputKey::Up},
        {A::Kind::Key, {}, NovaTerm::InputKey::Down},
        {A::Kind::Key, {}, NovaTerm::InputKey::Left},
        {A::Kind::Key, {}, NovaTerm::InputKey::Right},
        {A::Kind::Bytes, endingBytes.toStdString()},
    };
    QVERIFY(controller->submit("client", "raw-keys", actions, false));
    const QByteArray arrows = applicationCursor
        ? QByteArray("\x1bOA\x1bOB\x1bOD\x1bOC")
        : QByteArray("\x1b[A\x1b[B\x1b[D\x1b[C");
    const auto expected = QByteArray("raw\x03\t") + arrows + endingBytes;
    QTRY_COMPARE(transport.writes, expected);
    QTRY_VERIFY(!controller->pending());
    QVERIFY(!controller->commandActive());
    QVERIFY(!controller->readyForCommand());
    QCOMPARE(controller->operation().value("acceptedActionCount").toInt(), 8);

    // Enter 保留终端键盘语义；只有显式行结束动作使用配置的设备尾符。
    QVERIFY(controller->submit("client", "enter-key",
        {{A::Kind::Key, {}, NovaTerm::InputKey::Enter}}, false));
    QTRY_COMPARE(transport.writes, expected + '\r');
}

QTEST_MAIN(SessionTests)
#include "SessionTests.moc"
