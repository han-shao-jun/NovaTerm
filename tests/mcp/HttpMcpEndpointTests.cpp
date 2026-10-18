/** @file HttpMcpEndpointTests.cpp
 *  @brief HTTP 协议头、客户端隔离及两版断流语义的真实 TCP 回归。
 */
#include "mcp/HttpMcpEndpoint.h"
#include "mcp/protocol/BackendEvents.h"
#include "mcp/McpProtocol.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTest>
#include <QTcpSocket>
#include <thread>
#include <chrono>

using namespace NovaTerm::Mcp;
namespace {
struct Harness {
    QList<QPair<QString, BackendEvent>> messages;
    QSet<QString> peers;
    HttpMcpEndpoint endpoint{HttpMcpEndpoint::Backend{
        [](const QByteArray& token) { return token == "one" ? QString("client-one")
            : token == "two" ? QString("client-two") : QString{}; },
        [this](const QString& peer, const QString&) { peers.insert(peer); return true; },
        [this](const QString& peer, const BackendEvent& event) { messages.append({peer, event}); },
        [this](const QString& peer) { peers.remove(peer); }
    }};
    QNetworkAccessManager network;
    QUrl url;
    bool start()
    {
        QSignalSpy listening(&endpoint, &HttpMcpEndpoint::listening);
        endpoint.start(0);
        if (!listening.wait(3000) || !listening.first().first().toBool()) return false;
        url = QUrl(listening.first().at(1).toString());
        return true;
    }
    QNetworkReply* post(QJsonObject message, QByteArray session = {}, bool modern = false,
                        QByteArray token = "one", QByteArray methodOverride = {})
    {
        QNetworkRequest request{url};
        request.setTransferTimeout(std::chrono::seconds(5));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("Accept", "application/json, text/event-stream");
        request.setRawHeader("Authorization", "Bearer " + token);
        request.setRawHeader("MCP-Protocol-Version", modern ? "2026-07-28" : "2025-11-25");
        if (!session.isEmpty()) request.setRawHeader("Mcp-Session-Id", session);
        if (modern) {
            auto params = message.value("params").toObject();
            params.insert("_meta", QJsonObject{
                {"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}});
            message.insert("params", params);
            request.setRawHeader("Mcp-Method", methodOverride.isEmpty()
                ? message.value("method").toString().toUtf8() : methodOverride);
            if (message.value("method") == "tools/call")
                request.setRawHeader("Mcp-Name", message.value("params").toObject().value("name").toString().toUtf8());
        }
        return network.post(request, QJsonDocument(message).toJson(QJsonDocument::Compact));
    }
    QByteArray initialize()
    {
        auto* reply = post({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
            {"params", QJsonObject{{"protocolVersion", ProtocolVersion},
                {"capabilities", QJsonObject{}}, {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}}}}});
        QSignalSpy finished(reply, &QNetworkReply::finished);
        if (!reply->isFinished() && !finished.wait(3000)) return {};
        const auto session = reply->rawHeader("Mcp-Session-Id");
        reply->deleteLater();
        auto* notified = post({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, session);
        QSignalSpy notificationFinished(notified, &QNetworkReply::finished);
        if (!notified->isFinished() && !notificationFinished.wait(3000)) return {};
        notified->deleteLater();
        return session;
    }
};
QJsonObject toolCall(int id = 2)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"},
        {"params", QJsonObject{{"name", "novaterm_list_sessions"}, {"arguments", QJsonObject{}}}}};
}
int status(QNetworkReply* reply)
{ return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(); }
QString messageId(const BackendEvent& event)
{ return std::visit([](const auto& value) { return value.id; }, event); }
}
class HttpMcpEndpointTests final : public QObject
{
    Q_OBJECT
private slots:
    void modernNotificationDoesNotAllocateContext_data()
    {
        QTest::addColumn<QJsonValue>("capabilities");
        QTest::newRow("missing") << QJsonValue(QJsonValue::Undefined);
        QTest::newRow("null") << QJsonValue(QJsonValue::Null);
        QTest::newRow("string") << QJsonValue("invalid");
        QTest::newRow("array") << QJsonValue(QJsonArray{});
        QTest::newRow("object") << QJsonValue(QJsonObject{});
    }
    void modernNotificationDoesNotAllocateContext()
    {
        QFETCH(QJsonValue, capabilities);
        Harness h;
        QVERIFY(h.start());
        QJsonObject meta{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}};
        if (!capabilities.isUndefined())
            meta.insert("io.modelcontextprotocol/clientCapabilities", capabilities);
        const QJsonObject message{{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"},
            {"params", QJsonObject{{"requestId", 1}, {"_meta", meta}}}};
        QNetworkRequest request{h.url};
        request.setTransferTimeout(std::chrono::seconds(5));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("Accept", "application/json, text/event-stream");
        request.setRawHeader("Authorization", "Bearer one");
        request.setRawHeader("MCP-Protocol-Version", "2026-07-28");
        request.setRawHeader("Mcp-Method", "notifications/cancelled");
        auto* reply = h.network.post(request, QJsonDocument(message).toJson(QJsonDocument::Compact));
        QTRY_VERIFY(reply->isFinished());
        QCOMPARE(status(reply), 400);
        QCOMPARE(QJsonDocument::fromJson(reply->readAll()).object().value("error")
            .toObject().value("code").toInt(), -32600);
        QVERIFY(h.peers.isEmpty());
        reply->deleteLater();
    }
    void modernRequestCapabilitiesAreValidated_data()
    {
        QTest::addColumn<QJsonValue>("capabilities");
        QTest::newRow("missing") << QJsonValue(QJsonValue::Undefined);
        QTest::newRow("null") << QJsonValue(QJsonValue::Null);
        QTest::newRow("string") << QJsonValue("invalid");
        QTest::newRow("array") << QJsonValue(QJsonArray{});
    }
    void modernRequestCapabilitiesAreValidated()
    {
        QFETCH(QJsonValue, capabilities);
        Harness h;
        QVERIFY(h.start());
        QJsonObject meta{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}};
        if (!capabilities.isUndefined())
            meta.insert("io.modelcontextprotocol/clientCapabilities", capabilities);
        QNetworkRequest request{h.url};
        request.setTransferTimeout(std::chrono::seconds(5));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("Accept", "application/json, text/event-stream");
        request.setRawHeader("Authorization", "Bearer one");
        request.setRawHeader("MCP-Protocol-Version", "2026-07-28");
        request.setRawHeader("Mcp-Method", "server/discover");
        auto* reply = h.network.post(request, QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", 2},
            {"method", "server/discover"}, {"params", QJsonObject{{"_meta", meta}}}})
            .toJson(QJsonDocument::Compact));
        QTRY_VERIFY(reply->isFinished());
        QCOMPARE(status(reply), 400);
        QCOMPARE(QJsonDocument::fromJson(reply->readAll()).object().value("error")
            .toObject().value("code").toInt(), -32602);
        QVERIFY(h.peers.isEmpty());
        reply->deleteLater();
    }
    void invalidInitializeDoesNotReserveClient_data()
    {
        QTest::addColumn<QJsonObject>("message");
        const QJsonObject base{{"jsonrpc", "2.0"}, {"method", "initialize"},
            {"params", QJsonObject{{"protocolVersion", ProtocolVersion},
                {"capabilities", QJsonObject{}},
                {"clientInfo", QJsonObject{{"name", "fixture"}, {"version", "1"}}}}}};
        QTest::newRow("missing-id") << base;
        auto invalid = base;
        invalid.insert("id", QJsonValue(QJsonValue::Null));
        QTest::newRow("null-id") << invalid;
        invalid.insert("id", true);
        QTest::newRow("boolean-id") << invalid;
        invalid.insert("id", 1);
        invalid.insert("jsonrpc", "1.0");
        QTest::newRow("bad-jsonrpc") << invalid;
        invalid = base;
        invalid.insert("id", 1);
        invalid.remove("params");
        QTest::newRow("missing-params") << invalid;
        invalid.insert("params", QJsonArray{});
        QTest::newRow("array-params") << invalid;
    }
    void invalidInitializeDoesNotReserveClient()
    {
        QFETCH(QJsonObject, message);
        Harness h;
        QVERIFY(h.start());
        for (int i = 0; i < MaxClients; ++i) {
            auto* reply = h.post(message);
            QTRY_VERIFY(reply->isFinished());
            QCOMPARE(status(reply), 400);
            QVERIFY(reply->rawHeader("Mcp-Session-Id").isEmpty());
            QVERIFY(h.peers.isEmpty());
            reply->deleteLater();
        }
        QVERIFY(!h.initialize().isEmpty());
    }
    void invalidNotificationIsNotAccepted()
    {
        Harness h;
        QVERIFY(h.start());
        const auto session = h.initialize();
        QVERIFY(!session.isEmpty());
        auto* reply = h.post({{"jsonrpc", "1.0"}, {"method", "notifications/initialized"}}, session);
        QTRY_VERIFY(reply->isFinished());
        QCOMPARE(status(reply), 400);
        QCOMPARE(QJsonDocument::fromJson(reply->readAll()).object()
            .value("error").toObject().value("code").toInt(), -32600);
        QCOMPARE(h.peers.size(), 1);
        reply->deleteLater();
    }
    void disconnectedInitializeDoesNotReserveClient()
    {
        Harness h;
        QVERIFY(h.start());
        QTcpSocket socket;
        socket.connectToHost(h.url.host(), quint16(h.url.port()));
        QVERIFY(socket.waitForConnected(2000));
        const auto body = QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", 1},
            {"method", "initialize"}, {"params", QJsonObject{{"protocolVersion", ProtocolVersion},
                {"capabilities", QJsonObject{}}, {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}}}}})
            .toJson(QJsonDocument::Compact);
        socket.write("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(h.url.port())
            + "\r\nAuthorization: Bearer one\r\nContent-Type: application/json\r\n"
              "Accept: application/json, text/event-stream\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        socket.flush();
        // 刻意阻止 GUI drain，先让 I/O 接纳后确认连接已经断开。
        QElapsedTimer timer;
        timer.start();
        while (h.endpoint.statistics().value("acceptedRequests").toString() != "1"
            && timer.elapsed() < 2000)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        QCOMPARE(h.endpoint.statistics().value("acceptedRequests").toString(), QString("1"));
        socket.abort();
        while (h.endpoint.statistics().value("liveSockets").toInt() && timer.elapsed() < 2000)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        QCOMPARE(h.endpoint.statistics().value("liveSockets").toInt(), 0);
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(h.peers.isEmpty(), 1000);
        QVERIFY(!h.initialize().isEmpty());
    }
    void unsupportedVersionMatchesErrorSchema()
    {
        Harness h;
        QVERIFY(h.start());
        QNetworkRequest request{h.url};
        request.setTransferTimeout(std::chrono::seconds(5));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("Accept", "application/json, text/event-stream");
        request.setRawHeader("Authorization", "Bearer one");
        request.setRawHeader("MCP-Protocol-Version", "2099-01-01");
        auto* reply = h.network.post(request,
            "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"server/discover\"}");
        QTRY_VERIFY(reply->isFinished());
        QCOMPARE(status(reply), 400);
        const auto error = QJsonDocument::fromJson(reply->readAll()).object().value("error").toObject();
        QCOMPARE(error.value("code").toInt(), -32022);
        const auto data = error.value("data").toObject();
        QCOMPARE(data.value("requested").toString(), QString("2099-01-01"));
        QVERIFY(data.value("supported").toArray().contains("2026-07-28"));
        QVERIFY(!data.contains("supportedVersions"));
        QVERIFY(h.peers.isEmpty());
        reply->deleteLater();
    }
    void legacyHeaderCannotSelectModernBodySemantics()
    {
        Harness h;
        QVERIFY(h.start());
        const auto session = h.initialize();
        QVERIFY(!session.isEmpty());
        auto message = toolCall();
        auto params = message.value("params").toObject();
        params.insert("_meta", QJsonObject{
            {"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
            {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}});
        message.insert("params", params);
        auto* reply = h.post(message, session);
        QTRY_VERIFY(reply->isFinished());
        QCOMPARE(status(reply), 400);
        QCOMPARE(QJsonDocument::fromJson(reply->readAll()).object()
            .value("error").toObject().value("code").toInt(), -32020);
        reply->deleteLater();
        QVERIFY(h.messages.isEmpty());
    }
    void modernHeaderMismatchDoesNotDispatch()
    {
        Harness h;
        QVERIFY(h.start());
        auto* reply = h.post(toolCall(), {}, true, "one", "tools/list");
        QTRY_VERIFY(reply->isFinished());
        QCOMPARE(status(reply), 400);
        QCOMPARE(QJsonDocument::fromJson(reply->readAll()).object()
            .value("error").toObject().value("code").toInt(), -32020);
        reply->deleteLater();
        QVERIFY(h.messages.isEmpty());
        QVERIFY(h.peers.isEmpty());
    }
    void modernUnknownMethodUses404AndNoSession()
    {
        Harness h;
        QVERIFY(h.start());
        auto* reply = h.post({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "missing/method"}}, {}, true);
        QTRY_VERIFY(reply->isFinished());
        QCOMPARE(status(reply), 404);
        QVERIFY(reply->rawHeader("Mcp-Session-Id").isEmpty());
        QVERIFY(h.messages.isEmpty());
    }
    void legacySessionCannotBeUsedByOtherClient()
    {
        Harness h;
        QVERIFY(h.start());
        const auto session = h.initialize();
        QVERIFY(!session.isEmpty());
        auto* reply = h.post(toolCall(), session, false, "two");
        QTRY_VERIFY(reply->isFinished());
        QCOMPARE(status(reply), 404);
        QVERIFY(h.messages.isEmpty());
    }
    void identicalModernRpcIdsHaveIndependentBackendIds()
    {
        Harness h;
        QVERIFY(h.start());
        auto* first = h.post(toolCall(), {}, true);
        auto* second = h.post(toolCall(), {}, true);
        QTRY_COMPARE(h.messages.size(), 2);
        const auto a = h.messages.first();
        const auto b = h.messages.last();
        QCOMPARE(a.first, b.first);
        QVERIFY(messageId(a.second) != messageId(b.second));
        QVERIFY(h.endpoint.send(a.first, {{"op", "result"}, {"id", messageId(a.second)},
            {"payload", success({{"which", 1}})}}));
        QVERIFY(h.endpoint.send(b.first, {{"op", "result"}, {"id", messageId(b.second)},
            {"payload", success({{"which", 2}})}}));
        QTRY_VERIFY(first->isFinished() && second->isFinished());
        const auto firstBytes = first->readAll();
        const auto secondBytes = second->readAll();
        QVERIFY(firstBytes.contains("data:"));
        QVERIFY(secondBytes.contains("data:"));
        QVERIFY(firstBytes != secondBytes);
    }
    void modernDisconnectCancelsOnlyItsRequest()
    {
        Harness h;
        QVERIFY(h.start());
        auto* first = h.post(toolCall(2), {}, true);
        auto* second = h.post(toolCall(3), {}, true);
        QTRY_COMPARE(h.messages.size(), 2);
        first->abort();
        QTRY_COMPARE(h.messages.size(), 3);
        QCOMPARE(backendEventKind(h.messages.last().second), QString("cancel"));
        QCOMPARE(messageId(h.messages.last().second), messageId(h.messages.first().second));
        QVERIFY(!second->isFinished());
        second->abort();
    }
    void legacyDisconnectDoesNotCancelButDeleteDoes()
    {
        Harness h;
        QVERIFY(h.start());
        const auto session = h.initialize();
        QVERIFY(!session.isEmpty());
        auto* reply = h.post(toolCall(), session);
        QTRY_COMPARE(h.messages.size(), 1);
        reply->abort();
        QTest::qWait(100);
        QCOMPARE(h.messages.size(), 1);
        QNetworkRequest request{h.url};
        request.setTransferTimeout(std::chrono::seconds(5));
        request.setRawHeader("Authorization", "Bearer one");
        request.setRawHeader("Mcp-Session-Id", session);
        request.setRawHeader("MCP-Protocol-Version", "2025-11-25");
        auto* deleted = h.network.sendCustomRequest(request, "DELETE");
        QTRY_VERIFY(deleted->isFinished());
        QCOMPARE(status(deleted), 200);
        QCOMPARE(backendEventKind(h.messages.last().second), QString("cancel"));
        QVERIFY(h.peers.isEmpty());
    }
    void sessionLimitAndDeleteReturnCapacity()
    {
        Harness h;
        QVERIFY(h.start());
        QList<QByteArray> sessions;
        for (int i = 0; i < MaxClients; ++i) {
            sessions.append(h.initialize());
            QVERIFY(!sessions.last().isEmpty());
        }
        auto* overflow = h.post({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
            {"params", QJsonObject{{"protocolVersion", ProtocolVersion}, {"capabilities", QJsonObject{}},
                {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}}}}});
        QTRY_VERIFY(overflow->isFinished());
        QCOMPARE(status(overflow), 429);
        h.endpoint.closeConnection(QString::fromLatin1(sessions.first()));
        QVERIFY(!h.initialize().isEmpty());
    }
};
QTEST_GUILESS_MAIN(HttpMcpEndpointTests)
#include "HttpMcpEndpointTests.moc"
