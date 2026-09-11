#include "core/terminal/TerminalCore.h"
#include "credential/CredentialStore.h"
#include "profile/ProfileStore.h"
#include "session/SessionManager.h"
#include "session/SessionStore.h"
#include "session/TerminalSession.h"
#include "session/SessionInputPump.h"
#include "transport/ITransport.h"

#include <QSignalSpy>
#include <QKeyEvent>
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

private:
    bool _connected{false};
    bool _supportsReconnect{true};
};

class SessionTests final : public QObject
{
    Q_OBJECT
private slots:
    void lifecycleAndManagerCleanup();
    void enterReconnectsBySessionType();
    void managerReconnectsFailedSession();
    void customSessionWithoutCapabilityDoesNotReconnect();
    void runtimeConfigIsSnapshot();
    void restoreMetadataRoundTrip();
    void persistentStoresRejectSecrets();
    void reconnectBumpsGenerationAndKeepsHandlersLive();
    void structuredTransportErrorSetsSessionCategory();
    void agentContextFiltersProgressWrapDuplicatesAndAlternate();
    void inputPumpOffsetsPreservePendingSuffix();
    void agentContextJoinsHistorySeamAndBoundsUtf8();
};

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

void SessionTests::lifecycleAndManagerCleanup()
{
    RuntimeConfig config;
    config.title = QStringLiteral("test");
    auto session = std::make_unique<TerminalSession>(config);
    auto* transport = new FakeTransport;
    session->attach(transport);
    SessionManager manager;
    QSignalSpy removed(&manager, &SessionManager::sessionRemoved);
    const SessionId id = manager.add(std::move(session));
    QVERIFY(!id.isNull());
    QTRY_COMPARE(manager.find(id)->state(), SessionState::Running);
    QCOMPARE(manager.size(), 1);
    QVERIFY(manager.close(id));
    QTRY_COMPARE(removed.size(), 1);
    QCOMPARE(manager.size(), 0);
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

void SessionTests::managerReconnectsFailedSession()
{
    RuntimeConfig config;
    config.transportKind = TransportKind::Ssh;
    auto session = std::make_unique<TerminalSession>(config);
    auto* transport = new FakeTransport;
    session->attach(transport);

    SessionManager manager;
    const SessionId id = manager.add(std::move(session));
    QVERIFY(!id.isNull());
    QTRY_COMPARE(manager.find(id)->state(), SessionState::Running);

    transport->simulateRemoteDisconnect();
    QCOMPARE(manager.find(id)->state(), SessionState::Failed);
    QVERIFY(manager.reconnect(id));
    QTRY_COMPARE(manager.find(id)->state(), SessionState::Running);
    QCOMPARE(transport->connectAttempts, 2);
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
    QVERIFY(directory.isValid());
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

QTEST_MAIN(SessionTests)
#include "SessionTests.moc"
