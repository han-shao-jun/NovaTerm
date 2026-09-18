/** @file McpTests.cpp
 *  @brief MCP 协议、授权、跨客户端摘要及受限命令的离线端到端检查。
 */
#include "mcp/McpService.h"
#include "mcp/McpProtocol.h"
#include "mcp/CommandPolicy.h"
#include "transport/SshTransport.h"
#include "ui/widgets/McpSettingsDialog.h"
#include "ElaApplication.h"
#include "ElaCheckBox.h"
#include "ElaTreeWidget.h"
#include <QApplication>
#include <QFontDatabase>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QProcess>
#include <QFile>
#include <QDir>
#include <QTimer>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

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
        SshCommandResult result;
        result.requestId = request.requestId;
        result.connectionGeneration = request.generation;
        result.executionMayHaveStarted = true;
        result.terminationConfirmed = confirmed;
        result.outcome = confirmed ? SshCommandOutcome::Completed : SshCommandOutcome::Disconnected;
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
    bool start(const QString& runtime, const QByteArray& token, const QString& instance = {})
    {
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("NOVATERM_MCP_RUNTIME_DIR", runtime);
        environment.insert("NOVATERM_MCP_TOKEN", QString::fromLatin1(token));
        process.setProcessEnvironment(environment);
        process.start(QString::fromUtf8(NOVATERM_MCP_BRIDGE), instance.isEmpty()
            ? QStringList{} : QStringList{QStringLiteral("--instance"), instance});
        if (!process.waitForStarted(3000)) return false;
        send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
            {"params", QJsonObject{{"protocolVersion", ProtocolVersion}, {"capabilities", QJsonObject{}},
                {"clientInfo", QJsonObject{{"name", "NovaTerm fixture"}, {"version", "1"}}}}}});
        const auto response = next();
        if (response.value("result").toObject().value("protocolVersion") != ProtocolVersion) return false;
        send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
        return true;
    }
    void send(const QJsonObject& message)
    {
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
    void settingsDialogSeparatesReadAndCommandPermission();
    void duplicateIndexSurvivesEvictionAndReset();
    void protocolLifecycleAndOversizedInput();
    void instanceSelectionAndDisconnectStayExplicit();
};

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
    QCOMPARE(tools().size(), 5);
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
    QCOMPARE(host.next().value("result").toObject().value("tools").toArray().size(), 5);
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
    QVERIFY(SshTransportTestAccess::queuedCommand(*f.ssh).contains("'/usr/bin/uname'"));
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

void McpTests::settingsDialogSeparatesReadAndCommandPermission()
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
    tree->topLevelItem(0)->setCheckState(2, Qt::Checked);
    QTRY_VERIFY(f.service.access().canRead(f.client, f.service.directory().entries().first()));
    QVERIFY(f.service.access().commands(f.client, f.service.directory().entries().first()).isEmpty());
    QTest::qWait(10);
    tree->topLevelItem(0)->setCheckState(3, Qt::Checked);
    QTRY_COMPARE(f.service.access().commands(f.client, f.service.directory().entries().first()).size(), 4);
    QTest::qWait(10);
    tree->topLevelItem(0)->child(0)->setCheckState(3, Qt::Unchecked);
    QTRY_COMPARE(f.service.access().commands(f.client, f.service.directory().entries().first()).size(), 3);
    QTest::qWait(10);
    tree->topLevelItem(0)->setCheckState(2, Qt::Unchecked);
    QTRY_VERIFY(!f.service.access().canRead(f.client, f.service.directory().entries().first()));
    QVERIFY(f.service.access().commands(f.client, f.service.directory().entries().first()).isEmpty());
    QTest::qWait(20);
    const auto preview = qEnvironmentVariable("NOVATERM_MCP_UI_PREVIEW");
    if (!preview.isEmpty()) QVERIFY(dialog->grab().save(preview));
    dialog->close();
    QTRY_VERIFY(dialog.isNull());
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
    std::optional<SshCommandResult> result;
    QObject::connect(&transport, &SshTransport::boundedCommandFinished, &transport,
        [&](const SshCommandResult& received) { result = received; });
    if (!transport.connectToHost() || !waitFor([&] { return transport.isConnected(); }, 10000)) {
        std::fprintf(stderr, "Loopback SSH connection failed: %s\n", qPrintable(transport.errorString())); return 1;
    }
    int failures = 0;
    const auto run = [&](quint64 id, const QByteArray& command, SshCommandLimits limits, SshCommandOutcome outcome) {
        result.reset();
        const bool submitted = transport.executeBoundedCommand(id, command, limits);
        const bool received = waitFor([&] { return result && result->requestId == id; }, 6000);
        const bool ok = submitted && received && result->outcome == outcome
            && result->standardOutput.size() + result->standardError.size() <= limits.maxOutputBytes;
        std::printf("loopback command %llu: %s\n", static_cast<unsigned long long>(id), ok ? "PASS" : "FAIL");
        failures += !ok;
    };
    run(1, "fixture-ok", {64, 1000}, SshCommandOutcome::Completed);
    if (!result || !result->terminationConfirmed || result->exitCode != 0) ++failures;
    run(2, "fixture-nonzero", {64, 1000}, SshCommandOutcome::Failed);
    if (!result || !result->terminationConfirmed || result->exitCode != 42) ++failures;
    run(3, "fixture-limit", {64, 1000}, SshCommandOutcome::OutputLimit);
    if (!result || !result->outputTruncated) ++failures;
    run(4, "fixture-timeout", {64, 200}, SshCommandOutcome::TimedOut);
    if (!result || result->terminationConfirmed || !result->executionMayHaveStarted) ++failures;
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
    constexpr qsizetype byteCount = 64 * 1024 * 1024;
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
