/** @file WireSessionTests.cpp @brief 共享 MCP wire 的初始化、路由与取消回归。 */
#include "mcp/WireSession.h"
#include "mcp/McpProtocol.h"
#include <QSignalSpy>
#include <QJsonDocument>
#include <QTest>
using namespace NovaTerm::Mcp;
namespace {
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
    void encodedResultPreservesUnicodeAndOutputLimit()
    {
        WireSession wire;
        ready(wire);
        QSignalSpy backend(&wire, &WireSession::backendMessage);
        QSignalSpy packets(&wire, &WireSession::packet);
        const QString text = QString::fromUtf8("中文\r\n引号\"与反斜线\\");
        wire.receiveMessage(call(20), "unicode");
        wire.receiveBackend({{"op", "result"}, {"id", backend.last().at(0).toJsonObject().value("id")},
            {"payload", success({{"text", text}})}});
        const auto bytes = packets.last().at(1).toByteArray();
        QVERIFY(!bytes.contains('\n') && !bytes.contains('\r'));
        QCOMPARE(QJsonDocument::fromJson(bytes).object().value("result").toObject()
            .value("structuredContent").toObject().value("data").toObject().value("text").toString(), text);
        QCOMPARE(packets.last().at(3).toInt(), 0);
        wire.receiveMessage(call(21), "oversized");
        wire.receiveBackend({{"op", "result"}, {"id", backend.last().at(0).toJsonObject().value("id")},
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
        QObject::connect(&wire, &WireSession::backendMessage, &wire,
            [&wire, payload](const QJsonObject& message) {
                if (message.value("op") == "call")
                    wire.receiveBackend({{"op", "result"}, {"id", message.value("id")},
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
            .value("tools").toArray().size(), 7);
    }
    void asynchronousResultKeepsExchangeRoute()
    {
        WireSession wire("unique-prefix-");
        ready(wire);
        QSignalSpy backend(&wire, &WireSession::backendMessage);
        QSignalSpy packets(&wire, &WireSession::packet);
        wire.receiveMessage(call(7), "exchange-A");
        QCOMPARE(backend.size(), 1);
        const auto privateCall = backend.first().at(0).toJsonObject();
        QVERIFY(privateCall.value("id").toString().startsWith("unique-prefix-"));
        QVERIFY(wire.hasPendingCalls());
        wire.receiveBackend({{"op", "result"}, {"id", privateCall.value("id")},
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
        QSignalSpy backend(&wire, &WireSession::backendMessage);
        QSignalSpy packets(&wire, &WireSession::packet);
        QSignalSpy completed(&wire, &WireSession::completed);
        wire.receiveMessage(call(8), "cancelled-exchange");
        QCOMPARE(backend.size(), 1);
        const auto id = backend.first().at(0).toJsonObject().value("id");
        wire.receiveMessage({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"},
            {"params", QJsonObject{{"requestId", 8}}}});
        QCOMPARE(backend.last().at(0).toJsonObject().value("op").toString(), QStringLiteral("cancel"));
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
