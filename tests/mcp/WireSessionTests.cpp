/** @file WireSessionTests.cpp @brief 共享 MCP wire 的初始化、路由与取消回归。 */
#include "mcp/WireSession.h"
#include "mcp/McpProtocol.h"
#include "mcp/protocol/ToolCodec.h"
#include <QSignalSpy>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>
using namespace NovaTerm::Mcp;
namespace {
QString backendId(const QSignalSpy& spy, int index)
{
    return std::visit([](const auto& value) { return value.id; },
        spy.at(index).at(0).value<BackendEvent>());
}
QString backendOp(const QSignalSpy& spy, int index)
{
    return backendEventKind(spy.at(index).at(0).value<BackendEvent>());
}
QString backendScriptContent(const QSignalSpy& spy, int index)
{
    const auto event = spy.at(index).at(0).value<BackendEvent>();
    if (const auto* call = std::get_if<BackendCall>(&event))
        return std::get<RunScriptRequest>(call->request).scriptContent;
    return {};
}
QJsonObject initialize()
{
    return {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
        {"params", QJsonObject{{"protocolVersion", ProtocolVersion}, {"capabilities", QJsonObject{}},
            {"clientInfo", QJsonObject{{"name", "fixture"}, {"version", "1"}}}}}};
}
void ready(WireSession& wire)
{
    wire.receiveMessage(initialize(), "init");
    wire.receiveMessage({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    wire.setBackendAvailable(true);
}
QJsonObject call(int id)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"},
        {"params", QJsonObject{{"name", "novaterm_list_sessions"}, {"arguments", QJsonObject{}}}}};
}
}
class WireSessionTests final : public QObject
{
    Q_OBJECT
private slots:
    void bothHandshakesUseTheSameAgentInstructions() {
        WireSession legacy; QSignalSpy legacyPackets(&legacy, &WireSession::packet);
        legacy.receiveMessage(initialize(), "init");
        const auto a = QJsonDocument::fromJson(legacyPackets.last()[1].toByteArray()).object()
            .value("result").toObject().value("instructions").toString();
        WireSession modern; QSignalSpy modernPackets(&modern, &WireSession::packet);
        modern.receiveMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "server/discover"},
            {"params", QJsonObject{{"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}}, "discover");
        const auto b = QJsonDocument::fromJson(modernPackets.last()[1].toByteArray()).object()
            .value("result").toObject().value("instructions").toString();
        QCOMPARE(a, agentInstructions()); QCOMPARE(b, a);
        QVERIFY(a.contains("no per-command risk confirmation"));
        QVERIFY(a.contains("UART idle completion is unverified"));
        QVERIFY(a.contains("source=log")); QVERIFY(a.contains("Never rerun uncertain"));
        QVERIFY(a.contains("novaterm_wait_text")); QVERIFY(a.contains("novaterm_start_task"));
        QVERIFY(a.contains("novaterm_query_task")); QVERIFY(a.contains("novaterm_stop_task"));
        QVERIFY(a.contains("after human takeover"));
    }
    void historyCallsUseTheirOwnProtocolEvent() {
        WireSession wire; ready(wire); QSignalSpy backend(&wire, &WireSession::backendEvent);
        wire.receiveMessage({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
            {"params", QJsonObject{{"name", "novaterm_list_captures"}, {"arguments", QJsonObject{}}}}}, "history");
        QCOMPARE(backend.size(), 1); const auto event = backend.first().at(0).value<BackendEvent>();
        const auto* history = std::get_if<BackendHistoryCall>(&event); QVERIFY(history);
        QVERIFY(std::holds_alternative<ListCapturesRequest>(history->request));
        QVERIFY(!std::holds_alternative<BackendCall>(event));
    }
    void historyResponsesWithoutRequestStateDoNotBecomeConfirmation() {
        WireSession wire; wire.setBackendAvailable(true); QSignalSpy backend(&wire, &WireSession::backendEvent);
        wire.receiveMessage({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
            {"params", QJsonObject{{"name", "novaterm_list_captures"}, {"arguments", QJsonObject{}},
                {"inputResponses", QJsonObject{{"confirmed", true}}},
                {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                    {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}});
        QCOMPARE(backend.size(), 1); const auto event = backend.first().at(0).value<BackendEvent>();
        const auto* history = std::get_if<BackendHistoryCall>(&event); QVERIFY(history); QVERIFY(!history->confirmationSupplied);
    }
    void elicitationResponsesResolveOnce_data()
    {
        QTest::addColumn<QJsonObject>("response");
        QTest::addColumn<QString>("action");
        QTest::addColumn<bool>("confirmed");
        QTest::newRow("accept") << QJsonObject{{"result", QJsonObject{{"action", "accept"},
            {"content", QJsonObject{{"confirmed", true}}}}}} << QString("accept") << true;
        QTest::newRow("decline") << QJsonObject{{"result", QJsonObject{{"action", "decline"}}}}
            << QString("decline") << false;
        QTest::newRow("cancel") << QJsonObject{{"result", QJsonObject{{"action", "cancel"}}}}
            << QString("cancel") << false;
        QTest::newRow("client-error") << QJsonObject{{"error", QJsonObject{{"code", -32603}, {"message", "closed"}}}}
            << QString("cancel") << false;
        QTest::newRow("accept-without-confirmation") << QJsonObject{{"result", QJsonObject{{"action", "accept"}}}}
            << QString("decline") << false;
    }
    void elicitationResponsesResolveOnce()
    {
        QFETCH(QJsonObject, response);
        QFETCH(QString, action);
        QFETCH(bool, confirmed);
        WireSession wire;
        auto init = initialize();
        auto params = init.value("params").toObject();
        params.insert("capabilities", QJsonObject{{"elicitation", QJsonObject{{"form", QJsonObject{}}}}});
        init.insert("params", params);
        wire.receiveMessage(init);
        wire.receiveMessage({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
        wire.setBackendAvailable(true);
        QSignalSpy backend(&wire, &WireSession::backendEvent);
        QSignalSpy packets(&wire, &WireSession::packet);
        wire.receiveMessage(call(2), "original");
        QCOMPARE(backend.size(), 1);
        const auto id = backendId(backend, 0);
        wire.receiveBackend({{"op", "elicitation"}, {"id", id}, {"message", "Confirm execution"}});
        QCOMPARE(packets.size(), 1);
        const auto prompt = QJsonDocument::fromJson(packets.takeFirst().at(1).toByteArray()).object();
        QCOMPARE(prompt.value("method").toString(), QStringLiteral("elicitation/create"));
        response.insert("jsonrpc", "2.0");
        response.insert("id", prompt.value("id"));
        wire.receiveMessage(response);
        QCOMPARE(backend.size(), 2);
        const auto event = backend.last().at(0).value<BackendEvent>();
        const auto* resolution = std::get_if<BackendElicitationResult>(&event);
        QVERIFY(resolution);
        QCOMPARE(resolution->id, id);
        QCOMPARE(resolution->answer, action == "cancel" ? ConfirmationAnswer::Cancel
            : confirmed ? ConfirmationAnswer::Accept : ConfirmationAnswer::Decline);
        wire.receiveMessage(response);
        QCOMPARE(backend.size(), 2);
        QVERIFY(packets.isEmpty());
        wire.receiveBackend({{"op", "result"}, {"id", id}, {"payload", success({})}});
        QCOMPARE(packets.size(), 1);
        QVERIFY(!wire.hasPendingCalls());
        wire.receiveBackend({{"op", "result"}, {"id", id}, {"payload", success({})}});
        QCOMPARE(packets.size(), 1);
    }
    void duplicateActiveRpcIdKeepsOriginalCall()
    {
        WireSession wire;
        ready(wire);
        QSignalSpy backend(&wire, &WireSession::backendEvent);
        QSignalSpy packets(&wire, &WireSession::packet);
        wire.receiveMessage(call(7), "original");
        wire.receiveMessage(call(7), "duplicate");
        QCOMPARE(backend.size(), 1);
        QCOMPARE(packets.size(), 1);
        QCOMPARE(packets.first().at(0).toString(), QStringLiteral("duplicate"));
        const auto rejected = QJsonDocument::fromJson(packets.first().at(1).toByteArray()).object();
        QCOMPARE(rejected.value("error").toObject().value("code").toInt(), -32600);
        QVERIFY(wire.hasPendingCalls());
        wire.receiveBackend({{"op", "result"}, {"id", backendId(backend, 0)}, {"payload", success({})}});
        QCOMPARE(packets.size(), 2);
        QCOMPARE(packets.last().at(0).toString(), QStringLiteral("original"));
        QVERIFY(!wire.hasPendingCalls());
    }
    void waitingTerminalCallsKeepTheirObservationBudget_data()
    {
        QTest::addColumn<QString>("tool");
        QTest::addColumn<QJsonObject>("extra");
        QTest::addColumn<qint64>("expiry");
        QTest::newRow("compact-read") << QString("novaterm_read_context")
            << QJsonObject{{"responseMode", "compact"}, {"waitMs", 30000}} << qint64(35001);
        QTest::newRow("waiting-legacy-read") << QString("novaterm_read_context")
            << QJsonObject{{"waitMs", 30000}} << qint64(35001);
        QTest::newRow("command") << QString("novaterm_interact_terminal")
            << QJsonObject{{"actions", QJsonArray{QJsonObject{{"type", "command"}, {"command", "pwd"}}}}}
            << qint64(35001);
        QTest::newRow("key") << QString("novaterm_interact_terminal")
            << QJsonObject{{"actions", QJsonArray{QJsonObject{{"type", "key"}, {"key", "Enter"}}}}}
            << qint64(6001);
        QTest::newRow("elevation") << QString("novaterm_request_elevation")
            << QJsonObject{{"method", "sudo"}, {"mode", "command"}, {"command", "pwd"}}
            << qint64(35001);
    }
    void waitingTerminalCallsKeepTheirObservationBudget()
    {
        QFETCH(QString, tool);
        QFETCH(QJsonObject, extra);
        QFETCH(qint64, expiry);
        qint64 elapsed = 0;
        WireSession wire({}, nullptr, [&elapsed] { return elapsed; });
        ready(wire);
        QSignalSpy backend(&wire, &WireSession::backendEvent);
        QSignalSpy packets(&wire, &WireSession::packet);
        QJsonObject arguments{{"sessionId", "11111111-1111-1111-1111-111111111111"}, {"epoch", "epoch"}};
        if (tool != "novaterm_read_context") {
            arguments.insert("inputToken", "input-token");
            arguments.insert("operationId", "22222222-2222-2222-2222-222222222222");
        }
        for (auto it = extra.begin(); it != extra.end(); ++it) arguments.insert(it.key(), it.value());
        wire.receiveMessage({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
            {"params", QJsonObject{{"name", tool}, {"arguments", arguments}}}});
        QCOMPARE(backend.size(), 1);
        elapsed = 2500;
        QTest::qWait(150);
        QVERIFY(wire.hasPendingCalls());
        QVERIFY(packets.isEmpty());
        elapsed = expiry;
        QTRY_VERIFY_WITH_TIMEOUT(!wire.hasPendingCalls(), 1000);
        QCOMPARE(backendOp(backend, backend.size() - 1), QString("cancel"));
        QCOMPARE(packets.size(), 1);
    }
    void terminalActionsAreStrictlyValidated()
    {
        const QJsonObject identity{{"sessionId", "11111111-1111-1111-1111-111111111111"},
            {"epoch", "epoch"}, {"inputToken", "token"},
            {"operationId", "22222222-2222-2222-2222-222222222222"}};
        const auto validate = [&](const QJsonArray& actions) {
            auto args = identity;
            args.insert("actions", actions);
            return ToolCodec::validate("novaterm_interact_terminal", args);
        };
        QVERIFY(validate({QJsonObject{{"type", "text"}, {"text", "hello"}},
            QJsonObject{{"type", "key"}, {"key", "c"}, {"modifiers", QJsonArray{"Ctrl"}}}}).isEmpty());
        QVERIFY(validate({QJsonObject{{"type", "paste"}, {"text", "one\ntwo\tthree\r"}}}).isEmpty());
        QVERIFY(validate({QJsonObject{{"type", "key"}, {"key", QString::fromUtf8("😀")}}}).isEmpty());
        QVERIFY(!validate({}).isEmpty());
        QVERIFY(!validate({QJsonObject{{"type", "text"}, {"text", "bad\ntext"}}}).isEmpty());
        QVERIFY(!validate({QJsonObject{{"type", "text"}, {"text", QString(QChar(127))}}}).isEmpty());
        QVERIFY(!validate({QJsonObject{{"type", "paste"}, {"text", QString(QChar::Null)}}}).isEmpty());
        QVERIFY(!validate({QJsonObject{{"type", "key"}, {"key", "Enter"}, {"text", "extra"}}}).isEmpty());
        QVERIFY(!validate({QJsonObject{{"type", "key"}, {"key", "ab"}}}).isEmpty());
        QVERIFY(!validate({QJsonObject{{"type", "key"}, {"key", "c"}, {"modifiers", QJsonArray{"Meta"}}}}).isEmpty());
        QVERIFY(!validate({QJsonObject{{"type", "line_ending"}, {"text", "extra"}}}).isEmpty());
        QVERIFY(!validate({QJsonObject{{"type", "release"}}, QJsonObject{{"type", "line_ending"}}}).isEmpty());
        QVERIFY(!validate({QJsonObject{{"type", "command"}, {"command", "pwd"}}, QJsonObject{{"type", "line_ending"}}}).isEmpty());
        QVERIFY(!validate({QJsonObject{{"type", "command"}, {"command", "one\ntwo"}}}).isEmpty());
        QVERIFY(!validate({QJsonObject{{"type", "text"}, {"text", QString(16385, QLatin1Char('a'))}}}).isEmpty());
        QVERIFY(!validate({QJsonObject{{"type", "text"}, {"text", QString(8193, QChar(0x4e00))}}}).isEmpty());
        QJsonArray tooMany;
        for (int i = 0; i < 65; ++i) tooMany.append(QJsonObject{{"type", "line_ending"}});
        QVERIFY(!validate(tooMany).isEmpty());
        auto badIdentity = identity;
        badIdentity.insert("operationId", "not-a-uuid");
        badIdentity.insert("actions", QJsonArray{QJsonObject{{"type", "release"}}});
        QVERIFY(!ToolCodec::validate("novaterm_interact_terminal", badIdentity).isEmpty());
        auto elevation = identity;
        elevation.insert("method", "sudo");
        elevation.insert("mode", "shell");
        QVERIFY(ToolCodec::validate("novaterm_request_elevation", elevation).isEmpty());
        elevation.insert("command", "pwd");
        QVERIFY(!ToolCodec::validate("novaterm_request_elevation", elevation).isEmpty());
        elevation.insert("mode", "command");
        QVERIFY(ToolCodec::validate("novaterm_request_elevation", elevation).isEmpty());
        elevation.remove("command");
        QVERIFY(!ToolCodec::validate("novaterm_request_elevation", elevation).isEmpty());
        QJsonObject read{{"sessionId", identity.value("sessionId")}, {"epoch", "epoch"},
            {"expandToken", "expand"}, {"sinceToken", "since"}};
        QVERIFY(!ToolCodec::validate("novaterm_read_context", read).isEmpty());
    }
    void terminalSchemasDescribeStrictActionsAndCompactObservations()
    {
        QJsonObject interact, read, sessions;
        for (const auto& entry : ToolCodec::catalog()) {
            const auto tool = entry.toObject();
            if (tool.value("name") == "novaterm_interact_terminal") interact = tool;
            if (tool.value("name") == "novaterm_read_context") read = tool;
            if (tool.value("name") == "novaterm_list_sessions") sessions = tool;
        }
        QVERIFY(!interact.isEmpty());
        const auto actions = interact.value("inputSchema").toObject().value("properties").toObject()
            .value("actions").toObject();
        QCOMPARE(actions.value("maxItems").toInt(), 64);
        const auto variants = actions.value("items").toObject().value("oneOf").toArray();
        QCOMPARE(variants.size(), 6);
        for (const auto& variant : variants)
            QCOMPARE(variant.toObject().value("additionalProperties"), QJsonValue(false));
        const auto readData = read.value("outputSchema").toObject().value("properties").toObject()
            .value("data").toObject();
        const auto sources = readData.value("anyOf").toArray(); QCOMPARE(sources.size(), 2);
        const auto observation = sources.first().toObject();
        QVERIFY(observation.value("additionalProperties").toBool());
        QVERIFY(!observation.value("required").toArray().contains("viewport"));
        QVERIFY(observation.value("properties").toObject().contains("inputToken"));
        const auto log = sources.last().toObject();
        QCOMPARE(log.value("additionalProperties"), QJsonValue(false));
        QVERIFY(log.value("required").toArray().contains("fragments"));
        QVERIFY(log.value("required").toArray().contains("earliestToken"));
        const auto sessionItem = sessions.value("outputSchema").toObject().value("properties").toObject()
            .value("data").toObject().value("properties").toObject().value("sessions").toObject()
            .value("items").toObject();
        QVERIFY(sessionItem.value("properties").toObject().contains("permissions"));
    }
    void releaseDoesNotWaitForObservation()
    {
        qint64 elapsed = 0;
        WireSession wire({}, nullptr, [&elapsed] { return elapsed; });
        ready(wire);
        QSignalSpy backend(&wire, &WireSession::backendEvent);
        const QJsonObject arguments{{"sessionId", "11111111-1111-1111-1111-111111111111"},
            {"epoch", "epoch"}, {"inputToken", "token"}, {"waitMs", 30000},
            {"operationId", "22222222-2222-2222-2222-222222222222"},
            {"actions", QJsonArray{QJsonObject{{"type", "release"}}}}};
        wire.receiveMessage({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
            {"params", QJsonObject{{"name", "novaterm_interact_terminal"}, {"arguments", arguments}}}});
        QCOMPARE(backend.size(), 1);
        elapsed = 2001;
        QTRY_VERIFY_WITH_TIMEOUT(!wire.hasPendingCalls(), 1000);
    }
    void lateConfirmationLeavesFullExecutionBudget_data()
    {
        QTest::addColumn<QString>("tool");
        QTest::addColumn<qint64>("completionTime");
        QTest::addColumn<qint64>("expiryTime");
        QTest::newRow("command-30s-after-55s-confirmation")
            << QString("novaterm_run_command") << qint64(85000) << qint64(95001);
        QTest::newRow("script-confirmation-upload-and-execution")
            << QString("novaterm_run_script") << qint64(205000) << qint64(215001);
    }
    void lateConfirmationLeavesFullExecutionBudget()
    {
        QFETCH(QString, tool);
        QFETCH(qint64, completionTime);
        QFETCH(qint64, expiryTime);
        qint64 elapsed = 0;
        WireSession wire({}, nullptr, [&elapsed] { return elapsed; });
        auto init = initialize();
        auto params = init.value("params").toObject();
        params.insert("capabilities", QJsonObject{{"elicitation",
            QJsonObject{{"form", QJsonObject{}}}}});
        init.insert("params", params);
        wire.receiveMessage(init);
        wire.receiveMessage({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
        wire.setBackendAvailable(true);
        QSignalSpy backend(&wire, &WireSession::backendEvent);
        QSignalSpy packets(&wire, &WireSession::packet);
        QJsonObject arguments{{"sessionId", "11111111-1111-1111-1111-111111111111"},
            {"epoch", "epoch"}, {"timeoutMs", 30000}};
        if (tool == "novaterm_run_script") {
            arguments.insert("scriptContent", "echo hello");
            arguments.insert("targetPath", "/tmp/script.sh");
            arguments.insert("workingDirectory", "/tmp");
            arguments.insert("invocation", "sh /tmp/script.sh");
        } else
            arguments.insert("command", "echo hello");
        wire.receiveMessage({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
            {"params", QJsonObject{{"name", tool}, {"arguments", arguments}}}}, "execute");
        QCOMPARE(backend.size(), 1);
        const auto id = backendId(backend, 0);
        wire.receiveBackend({{"op", "elicitation"}, {"id", id}, {"message", "Confirm execution"}});
        QCOMPARE(packets.size(), 1);
        const auto confirmation = QJsonDocument::fromJson(packets.takeFirst().at(1).toByteArray()).object();
        elapsed = 55000;
        wire.receiveMessage({{"jsonrpc", "2.0"}, {"id", confirmation.value("id")},
            {"result", QJsonObject{{"action", "accept"},
                {"content", QJsonObject{{"confirmed", true}}}}}});
        QCOMPARE(backendOp(backend, backend.size() - 1), QString("elicitation_result"));
        elapsed = completionTime;
        // 只等待100ms扫描器一次唤醒；55秒确认及120秒上传由单调时钟推进。
        QTest::qWait(150);
        QVERIFY2(wire.hasPendingCalls(), "Wire deadline cancelled an operation within its phase budgets");
        QVERIFY(packets.isEmpty());
        // 时钟扫描确实生效，且总预算耗尽时仍会取消，不能靠停用扫描器通过回归。
        elapsed = expiryTime;
        QTRY_VERIFY_WITH_TIMEOUT(!wire.hasPendingCalls(), 1000);
        QCOMPARE(packets.size(), 1);
        QCOMPARE(backendOp(backend, backend.size() - 1), QString("cancel"));
        const auto expired = QJsonDocument::fromJson(packets.takeFirst().at(1).toByteArray()).object()
            .value("result").toObject().value("structuredContent").toObject();
        QCOMPARE(expired.value("error").toObject().value("code").toString(),
            QString("COMMAND_OUTCOME_UNKNOWN"));
        // 下次请求在完整预算内收到实际结果，仍正常完成。
        wire.receiveMessage({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
            {"params", QJsonObject{{"name", tool}, {"arguments", arguments}}}}, "next-execute");
        const auto nextId = backendId(backend, backend.size() - 1);
        elapsed += completionTime;
        QTest::qWait(150);
        QVERIFY(wire.hasPendingCalls());
        wire.receiveBackend({{"op", "result"}, {"id", nextId}, {"payload", success({{"done", true}})}});
        QCOMPARE(packets.size(), 1);
        QVERIFY(!wire.hasPendingCalls());
        const auto result = QJsonDocument::fromJson(packets.first().at(1).toByteArray()).object()
            .value("result").toObject().value("structuredContent").toObject();
        QVERIFY(result.value("ok").toBool());
    }
    void packetCarriesEncodedJsonAndErrorMetadata()
    {
        WireSession wire;
        QSignalSpy packets(&wire, &WireSession::packet);
        wire.receiveMessage({{"jsonrpc", "1.0"}, {"id", 9}, {"method", "ping"}}, "invalid-message");
        QCOMPARE(packets.size(), 1);
        const auto packet = packets.first();
        QCOMPARE(packet.at(1).userType(), int(QMetaType::QByteArray));
        const auto bytes = packet.at(1).toByteArray();
        QVERIFY(QJsonDocument::fromJson(bytes).isObject());
        QCOMPARE(packet.at(0).toString(), QString("invalid-message"));
        QCOMPARE(packet.at(3).toInt(), -32600);
        QVERIFY(packet.at(2).toBool());
    }
    void largestEscapedScriptReachesBackend()
    {
        WireSession wire;
        ready(wire);
        QSignalSpy backend(&wire, &WireSession::backendEvent);
        QSignalSpy packets(&wire, &WireSession::packet);
        const QString content(MaxScriptBytes, QChar(1));
        const QJsonObject arguments{{"sessionId", "11111111-1111-1111-1111-111111111111"},
            {"epoch", "epoch"}, {"scriptContent", content}, {"targetPath", "/tmp/script.sh"},
            {"workingDirectory", "/tmp"}, {"invocation", "sh /tmp/script.sh"}};
        const auto message = QJsonObject{{"jsonrpc", "2.0"}, {"id", 22}, {"method", "tools/call"},
            {"params", QJsonObject{{"name", "novaterm_run_script"}, {"arguments", arguments}}}};
        const auto bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
        QVERIFY(bytes.size() > MaxFrameBytes);
        QVERIFY(bytes.size() <= MaxRequestBytes);
        wire.receiveBytes(bytes, "large-script");
        QCOMPARE(backend.size(), 1);
        QCOMPARE(backendScriptContent(backend, 0), content);
        QVERIFY(packets.isEmpty());
    }
    void oversizedRequestsAreRejectedBeforeDispatch()
    {
        WireSession wire;
        ready(wire);
        QSignalSpy backend(&wire, &WireSession::backendEvent);
        QSignalSpy packets(&wire, &WireSession::packet);
        wire.receiveBytes(QByteArray(MaxRequestBytes + 1, 'x'), "too-large-bytes");
        QCOMPARE(packets.size(), 1);
        QCOMPARE(packets.last().at(0).toString(), QString("too-large-bytes"));
        QCOMPARE(packets.last().at(3).toInt(), -32600);
        wire.receiveMessage({{"jsonrpc", "2.0"}, {"id", 23}, {"method", "ping"},
            {"params", QJsonObject{{"padding", QString(MaxRequestBytes, QLatin1Char('x'))}}}},
            "too-large-object");
        QCOMPARE(packets.size(), 2);
        QCOMPARE(packets.last().at(0).toString(), QString("too-large-object"));
        QCOMPARE(packets.last().at(3).toInt(), -32600);
        QVERIFY(backend.isEmpty());
    }
    void encodedResultPreservesUnicodeAndOutputLimit()
    {
        WireSession wire;
        ready(wire);
        QSignalSpy backend(&wire, &WireSession::backendEvent);
        QSignalSpy packets(&wire, &WireSession::packet);
        const QString text = QString::fromUtf8("中文\r\n引号\"与反斜线\\");
        wire.receiveMessage(call(20), "unicode");
        wire.receiveBackend({{"op", "result"}, {"id", backendId(backend, backend.size() - 1)},
            {"payload", success({{"text", text}})}});
        const auto bytes = packets.last().at(1).toByteArray();
        QVERIFY(!bytes.contains('\n') && !bytes.contains('\r'));
        QCOMPARE(QJsonDocument::fromJson(bytes).object().value("result").toObject()
            .value("structuredContent").toObject().value("data").toObject().value("text").toString(), text);
        QCOMPARE(packets.last().at(3).toInt(), 0);
        wire.receiveMessage(call(21), "oversized");
        wire.receiveBackend({{"op", "result"}, {"id", backendId(backend, backend.size() - 1)},
            {"payload", success({{"text", QString(MaxFrameBytes, QLatin1Char('x'))}})}});
        const auto bounded = packets.last().at(1).toByteArray();
        QVERIFY(bounded.size() <= MaxFrameBytes);
        QCOMPARE(QJsonDocument::fromJson(bounded).object().value("result").toObject()
            .value("structuredContent").toObject().value("error").toObject().value("code").toString(),
            QString("RESPONSE_TOO_LARGE"));
    }
    void benchmarkLargeToolResult()
    {
        WireSession wire;
        ready(wire);
        const auto payload = success({{"text", QString(256 * 1024, QLatin1Char('x'))}});
        QObject::connect(&wire, &WireSession::backendEvent, &wire,
            [&wire, payload](const BackendEvent& event) {
                if (const auto* call = std::get_if<BackendCall>(&event))
                    wire.receiveBackend({{"op", "result"}, {"id", call->id},
                        {"payload", payload}});
            });
        int count = 0;
        QObject::connect(&wire, &WireSession::packet, &wire,
            [&count](const QString&, const QByteArray&, bool, int) { ++count; });
        int id = 100;
        QBENCHMARK {
            wire.receiveMessage(call(++id), "large-response");
        }
        QVERIFY(count > 0);
    }
    void initializationAndCatalog()
    {
        WireSession wire;
        QSignalSpy packets(&wire, &WireSession::packet);
        wire.receiveMessage(initialize(), "http-initialize");
        QCOMPARE(packets.size(), 1);
        QCOMPARE(packets.first().at(0).toString(), QStringLiteral("http-initialize"));
        QCOMPARE(QJsonDocument::fromJson(packets.first().at(1).toByteArray()).object().value("result").toObject()
            .value("protocolVersion").toString(), QString::fromLatin1(ProtocolVersion));
        QVERIFY(!wire.initialized());
        wire.receiveMessage({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
        QVERIFY(wire.initialized());
        wire.receiveMessage({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}}, "catalog");
        QCOMPARE(QJsonDocument::fromJson(packets.last().at(1).toByteArray()).object().value("result").toObject()
            .value("tools").toArray().size(), 17);
    }
    void asynchronousResultKeepsExchangeRoute()
    {
        WireSession wire("unique-prefix-");
        ready(wire);
        QSignalSpy backend(&wire, &WireSession::backendEvent);
        QSignalSpy packets(&wire, &WireSession::packet);
        wire.receiveMessage(call(7), "exchange-A");
        QCOMPARE(backend.size(), 1);
        const auto privateId = backendId(backend, 0);
        QVERIFY(privateId.startsWith("unique-prefix-"));
        QVERIFY(wire.hasPendingCalls());
        wire.receiveBackend({{"op", "result"}, {"id", privateId},
            {"payload", success({{"value", "ok"}})}});
        QCOMPARE(packets.size(), 1);
        QCOMPARE(packets.first().at(0).toString(), QStringLiteral("exchange-A"));
        QCOMPARE(QJsonDocument::fromJson(packets.first().at(1).toByteArray()).object().value("id"), QJsonValue(7));
        QVERIFY(packets.first().at(2).toBool());
        QVERIFY(!wire.hasPendingCalls());
    }
    void cancelledCallNeverProducesNormalResult()
    {
        WireSession wire;
        ready(wire);
        QSignalSpy backend(&wire, &WireSession::backendEvent);
        QSignalSpy packets(&wire, &WireSession::packet);
        QSignalSpy completed(&wire, &WireSession::completed);
        wire.receiveMessage(call(8), "cancelled-exchange");
        QCOMPARE(backend.size(), 1);
        const auto id = backendId(backend, 0);
        wire.receiveMessage({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"},
            {"params", QJsonObject{{"requestId", 8}}}});
        QCOMPARE(backendOp(backend, backend.size() - 1), QStringLiteral("cancel"));
        QCOMPARE(completed.size(), 1);
        QCOMPARE(completed.first().at(0).toString(), QStringLiteral("cancelled-exchange"));
        wire.receiveBackend({{"op", "result"}, {"id", id}, {"payload", success({})}});
        QVERIFY(packets.isEmpty());
    }
    void modernDiscoverDoesNotRequireInitialization()
    {
        WireSession wire;
        QSignalSpy packets(&wire, &WireSession::packet);
        wire.receiveMessage({{"jsonrpc", "2.0"}, {"id", 9}, {"method", "server/discover"},
            {"params", QJsonObject{{"_meta", QJsonObject{
                {"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}}, "modern");
        QCOMPARE(packets.size(), 1);
        QCOMPARE(QJsonDocument::fromJson(packets.first().at(1).toByteArray()).object().value("result").toObject()
            .value("resultType").toString(), QStringLiteral("complete"));
    }
    void invalidJsonKeepsItsHttpRoute()
    {
        WireSession wire;
        QSignalSpy packets(&wire, &WireSession::packet);
        wire.receiveBytes("{", "malformed");
        QCOMPARE(packets.size(), 1);
        QCOMPARE(packets.first().at(0).toString(), QStringLiteral("malformed"));
        QCOMPARE(QJsonDocument::fromJson(packets.first().at(1).toByteArray()).object().value("error").toObject().value("code").toInt(), -32700);
    }
};
QTEST_GUILESS_MAIN(WireSessionTests)
#include "WireSessionTests.moc"
