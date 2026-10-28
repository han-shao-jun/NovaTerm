/** @file HttpMcpServerTests.cpp
 *  @brief 使用临时 loopback 端口验收 HTTP 分片、边界与生命周期。
 */
#include "mcp/HttpMcpServer.h"
#include "mcp/McpProtocol.h"
#include "mcp/protocol/ToolCodec.h"
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QNetworkProxy>
#include <QTest>
#include <QUrl>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <thread>
#include <vector>

using NovaTerm::Mcp::HttpMcpServer;

namespace {
int listen(HttpMcpServer& server)
{
    QSignalSpy listening(&server, &HttpMcpServer::listening);
    server.start();
    if (listening.isEmpty() && !listening.wait(3000)) return -1;
    return listening.first().at(0).toBool()
        ? QUrl(listening.first().at(1).toString()).port() : -1;
}

QByteArray getRequest(int port, QByteArray method = "GET", QByteArray extra = {})
{
    return method + " /mcp HTTP/1.1\r\nHost: localhost:"
        + QByteArray::number(port) + "\r\n" + extra + "\r\n";
}

QByteArray collect(QTcpSocket& socket)
{
    QByteArray result;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        if (socket.bytesAvailable()) result += socket.readAll();
        if (socket.state() == QAbstractSocket::UnconnectedState) break;
        socket.waitForReadyRead(10);
    }
    if (socket.bytesAvailable()) result += socket.readAll();
    return result;
}
}

class HttpMcpServerTests : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy); }
    void ssePreservesEncodedJsonAndBoundsMultilineFraming();
    void fragmentedUtf8PostReturnsJson();
    void rejectsMalformedHttp_data();
    void rejectsMalformedHttp();
    void chunkedPostAndJsonErrors_data();
    void chunkedPostAndJsonErrors();
    void sseStreamsChunksAndTerminates();
    void emptyResponsesAndRestart();
    void responseCreditAndHeaderInjectionAreRejected();
    void socketLimitAndStopWithoutGuiRoundtrip();
    void fullSizeBodyAndRepeatedSmallChunks();
    void requestFloodWhileGuiBlockedIsBounded();
    void blockedClientOutputEventuallyRejects();
    void rapidStartStopDoesNotNeedGuiEvents();
    void rejectedSocketsReturnResourcesToBaseline();
    void sameSourcePortPeersRemainIndependent();
    void closingSameSourcePortPeerKeepsOtherPeer();
    void sameSourcePortSocketsRespectLimit();
    void expectContinueBeforeBody_data();
    void expectContinueBeforeBody();
    void stopResetsLiveSocketsAndPreservesPeak();
    void parsedRequestRejectionDrainsResponse_data();
    void parsedRequestRejectionDrainsResponse();
};

void HttpMcpServerTests::ssePreservesEncodedJsonAndBoundsMultilineFraming()
{
    HttpMcpServer server;
    const auto port = listen(server);
    QVERIFY(port > 0);
    QSignalSpy requests(&server, &HttpMcpServer::request);
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, quint16(port));
    QVERIFY(socket.waitForConnected());
    socket.write(getRequest(port));
    socket.flush();
    QTRY_COMPARE(requests.size(), 1);
    const QByteArray json = "{\n  \"value\": \"unicode\\ntext\"\n}";
    QVERIFY(server.respond(requests.first().at(0).toString(), 200, json, {}, true, true));
    const auto response = collect(socket);
    QVERIFY(response.contains("data:{\ndata:  \"value\": \"unicode\\ntext\"\ndata:}\n\n"));
    QTRY_COMPARE(server.statistics().value("outputBytes").toInt(), 0);
    QTRY_COMPARE(server.statistics().value("liveSockets").toInt(), 0);
}

void HttpMcpServerTests::parsedRequestRejectionDrainsResponse_data()
{
    QTest::addColumn<QByteArray>("target");
    QTest::addColumn<QByteArray>("method");
    QTest::addColumn<int>("status");
    QTest::newRow("unknown-path") << QByteArray("/unknown") << QByteArray("GET") << 404;
    QTest::newRow("query") << QByteArray("/mcp?x=1") << QByteArray("GET") << 404;
    QTest::newRow("unsupported-method") << QByteArray("/mcp") << QByteArray("PUT") << 405;
}

void HttpMcpServerTests::parsedRequestRejectionDrainsResponse()
{
    QFETCH(QByteArray, target);
    QFETCH(QByteArray, method);
    QFETCH(int, status);
    HttpMcpServer server;
    const int port = listen(server);
    QVERIFY(port > 0);
    QSignalSpy requests(&server, &HttpMcpServer::request);
    QTcpSocket socket;
    socket.connectToHost("127.0.0.1", quint16(port));
    QVERIFY(socket.waitForConnected());
    socket.write(method + " " + target + " HTTP/1.1\r\nHost: localhost:"
        + QByteArray::number(port) + "\r\n\r\n");
    socket.flush();
    const auto response = collect(socket);
    QVERIFY2(response.startsWith("HTTP/1.1 " + QByteArray::number(status) + " "), response.constData());
    QVERIFY(response.endsWith("\r\n\r\n"));
    QCOMPARE(requests.size(), 0);
    QTRY_COMPARE(server.statistics().value("liveSockets").toInt(), 0);
    QCOMPARE(server.statistics().value("inputBytes").toInt(), 0);
}

void HttpMcpServerTests::sameSourcePortPeersRemainIndependent()
{
    HttpMcpServer server;
    const int port = listen(server);
    QVERIFY(port > 0);
    QSignalSpy requests(&server, &HttpMcpServer::request);
    QTcpSocket first;
    QTcpSocket second;
    QVERIFY(first.bind(QHostAddress("127.0.0.1"), 0));
    first.connectToHost("127.0.0.1", quint16(port));
    QVERIFY(first.waitForConnected());
    QVERIFY(second.bind(QHostAddress("127.0.0.2"), first.localPort()));
    second.connectToHost("127.0.0.1", quint16(port));
    QVERIFY(second.waitForConnected());
    QTRY_COMPARE(server.statistics().value("liveSockets").toInt(), 2);
    first.write(getRequest(port, "GET", "X-Peer: first\r\n"));
    second.write(getRequest(port, "GET", "X-Peer: second\r\n"));
    first.flush();
    second.flush();
    QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 2, 2000);
    for (const auto &request : requests) {
        const auto name = request.at(1).toJsonObject().value("headers").toObject()
                              .value("x-peer").toString();
        const auto body = name == "first" ? QByteArray("{\"peer\":1}") : QByteArray("{\"peer\":2}");
        QVERIFY(server.respond(request.at(0).toString(), 200, body));
    }
    QVERIFY(collect(first).endsWith("{\"peer\":1}"));
    QVERIFY(collect(second).endsWith("{\"peer\":2}"));
    QTRY_COMPARE(server.statistics().value("liveSockets").toInt(), 0);
}

void HttpMcpServerTests::closingSameSourcePortPeerKeepsOtherPeer()
{
    HttpMcpServer server;
    const int port = listen(server);
    QVERIFY(port > 0);
    QSignalSpy requests(&server, &HttpMcpServer::request);
    QTcpSocket first;
    QTcpSocket second;
    QVERIFY(first.bind(QHostAddress("127.0.0.1"), 0));
    first.connectToHost("127.0.0.1", quint16(port));
    QVERIFY(first.waitForConnected());
    QVERIFY(second.bind(QHostAddress("127.0.0.2"), first.localPort()));
    second.connectToHost("127.0.0.1", quint16(port));
    QVERIFY(second.waitForConnected());
    QTRY_COMPARE(server.statistics().value("liveSockets").toInt(), 2);
    first.abort();
    QTRY_COMPARE(server.statistics().value("liveSockets").toInt(), 1);
    second.write(getRequest(port));
    second.flush();
    QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 1, 2000);
    QVERIFY(server.respond(requests.first().at(0).toString(), 202));
    QVERIFY(collect(second).startsWith("HTTP/1.1 202 "));
}

void HttpMcpServerTests::sameSourcePortSocketsRespectLimit()
{
    HttpMcpServer server;
    const int port = listen(server);
    QVERIFY(port > 0);
    std::vector<std::unique_ptr<QTcpSocket>> sockets;
    for (int i = 0; i < 16; ++i) {
        auto socket = std::make_unique<QTcpSocket>();
        const auto address = QHostAddress(i % 2 ? "127.0.0.2" : "127.0.0.1");
        const auto sourcePort = i % 2 ? sockets.back()->localPort() : quint16(0);
        QVERIFY(socket->bind(address, sourcePort));
        socket->connectToHost("127.0.0.1", quint16(port));
        QVERIFY(socket->waitForConnected());
        sockets.push_back(std::move(socket));
    }
    QTRY_COMPARE(server.statistics().value("liveSockets").toInt(), 16);
    QTcpSocket overflow;
    overflow.connectToHost("127.0.0.1", quint16(port));
    QVERIFY(overflow.waitForConnected());
    collect(overflow);
    QCOMPARE(overflow.state(), QAbstractSocket::UnconnectedState);
    QCOMPARE(server.statistics().value("peakSockets").toInt(), 16);
}

void HttpMcpServerTests::expectContinueBeforeBody_data()
{
    QTest::addColumn<bool>("chunked");
    QTest::addColumn<QByteArray>("expectation");
    QTest::newRow("content-length") << false << QByteArray("100-continue");
    QTest::newRow("chunked") << true << QByteArray("100-continue");
    QTest::newRow("case-insensitive") << false << QByteArray("100-CoNtInUe");
}

void HttpMcpServerTests::expectContinueBeforeBody()
{
    QFETCH(bool, chunked);
    QFETCH(QByteArray, expectation);
    HttpMcpServer server;
    const int port = listen(server);
    QVERIFY(port > 0);
    QSignalSpy requests(&server, &HttpMcpServer::request);
    QTcpSocket socket;
    socket.connectToHost("127.0.0.1", quint16(port));
    QVERIFY(socket.waitForConnected());
    const QByteArray body = "{\"id\":100}";
    const auto framing = chunked ? QByteArray::number(body.size(), 16) + "\r\n" + body + "\r\n0\r\n\r\n"
                                : body;
    const auto lengthHeader = chunked ? QByteArray("Transfer-Encoding: chunked\r\n")
                                     : "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    const auto header = getRequest(port, "POST", lengthHeader + "Expect: " + expectation + "\r\n");
    socket.write(header.left(header.size() - 2));
    socket.flush();
    QTest::qWait(10);
    QCOMPARE(socket.bytesAvailable(), 0);
    socket.write(header.right(2));
    socket.flush();
    QByteArray response;
    QTRY_VERIFY_WITH_TIMEOUT((response += socket.readAll()).contains("HTTP/1.1 100 Continue\r\n\r\n"), 2000);
    QCOMPARE(requests.size(), 0);
    QVERIFY(server.statistics().value("inputBytes").toInt() >= 3 * header.size());
    socket.write(framing.left(framing.size() - 1));
    socket.flush();
    QTest::qWait(10);
    QCOMPARE(requests.size(), 0);
    socket.write(framing.right(1));
    socket.flush();
    QTRY_COMPARE(requests.size(), 1);
    const auto envelope = requests.first().at(1).toJsonObject();
    QCOMPARE(envelope.value("message").toObject().value("id").toInt(), 100);
    QCOMPARE(envelope.value("headers").toObject().value("expect").toString(), QString::fromLatin1(expectation));
    QVERIFY(server.respond(requests.first().at(0).toString(), 202));
    response += collect(socket);
    QCOMPARE(response.count("HTTP/1.1 100 Continue\r\n\r\n"), 1);
    QVERIFY(response.contains("HTTP/1.1 202 Accepted\r\n"));
    QTRY_COMPARE(server.statistics().value("inputBytes").toInt(), 0);
}

void HttpMcpServerTests::stopResetsLiveSocketsAndPreservesPeak()
{
    HttpMcpServer server;
    int port = listen(server);
    QVERIFY(port > 0);
    std::vector<std::unique_ptr<QTcpSocket>> sockets;
    for (int i = 0; i < 3; ++i) {
        auto socket = std::make_unique<QTcpSocket>();
        socket->connectToHost("127.0.0.1", quint16(port));
        QVERIFY(socket->waitForConnected());
        socket->write("POST /mcp HTTP/1.1\r\n");
        socket->flush();
        sockets.push_back(std::move(socket));
    }
    QTRY_COMPARE(server.statistics().value("liveSockets").toInt(), 3);
    QTRY_VERIFY(server.statistics().value("inputBytes").toInt() > 0);
    server.stop();
    QCOMPARE(server.statistics().value("liveSockets").toInt(), 0);
    QCOMPARE(server.statistics().value("peakSockets").toInt(), 3);
    QCOMPARE(server.statistics().value("inputBytes").toInt(), 0);
    port = listen(server);
    QVERIFY(port > 0);
    QTcpSocket restarted;
    restarted.connectToHost("127.0.0.1", quint16(port));
    QVERIFY(restarted.waitForConnected());
    QTRY_COMPARE(server.statistics().value("liveSockets").toInt(), 1);
    server.stop();
    QCOMPARE(server.statistics().value("liveSockets").toInt(), 0);
    QCOMPARE(server.statistics().value("peakSockets").toInt(), 3);
}

void HttpMcpServerTests::rejectedSocketsReturnResourcesToBaseline()
{
    HttpMcpServer server;
    const auto port = listen(server);
    QVERIFY(port > 0);
    for (int i = 0; i < 20; ++i) {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, quint16(port));
        QVERIFY(socket.waitForConnected(3000));
        socket.write("GET /mcp HTTP/1.1\r\nHost: untrusted.example\r\n\r\n");
        QVERIFY(collect(socket).startsWith("HTTP/1.1 400"));
    }
    QTRY_COMPARE(server.statistics().value("liveSockets").toInt(), 0);
    QCOMPARE(server.statistics().value("inputBytes").toInt(), 0);
    QCOMPARE(server.statistics().value("rejectedRequests").toString(), QString("20"));
}

void HttpMcpServerTests::fragmentedUtf8PostReturnsJson()
{
    HttpMcpServer server;
    QSignalSpy listening(&server, &HttpMcpServer::listening);
    QSignalSpy requests(&server, &HttpMcpServer::request);
    QSignalSpy disconnected(&server, &HttpMcpServer::disconnected);
    server.start();
    QTRY_COMPARE(listening.size(), 1);
    QVERIFY(listening.first().at(0).toBool());
    const QUrl url(listening.first().at(1).toString());
    QCOMPARE(url.path(), QStringLiteral("/mcp"));
    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), quint16(url.port()));
    QVERIFY(socket.waitForConnected());
    const QByteArray body = QStringLiteral("{\"jsonrpc\":\"2.0\",\"text\":\"中文\"}").toUtf8();
    const auto header = QByteArray("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:")
        + QByteArray::number(url.port()) + "\r\nContent-Length: "
        + QByteArray::number(body.size()) + "\r\n\r\n";
    socket.write(header.left(9));
    socket.flush();
    QTest::qWait(10);
    QCOMPARE(requests.size(), 0);
    socket.write(header.mid(9) + body.left(body.size() - 4));
    socket.flush();
    QTest::qWait(10);
    QCOMPARE(requests.size(), 0);
    socket.write(body.right(4));
    socket.flush();
    QTRY_COMPARE(requests.size(), 1);
    const auto envelope = requests.first().at(1).toJsonObject();
    QCOMPARE(envelope.value("method").toString(), QStringLiteral("POST"));
    QCOMPARE(envelope.value("message").toObject().value("text").toString(), QStringLiteral("中文"));
    const QString id = requests.first().at(0).toString();
    QVERIFY(server.respond(id, 200, "{\"ok\":true}", {{"MCP-Session-Id", "ticket"}}));
    QByteArray response;
    QTRY_VERIFY_WITH_TIMEOUT((response += socket.readAll()).contains("{\"ok\":true}"), 3000);
    QVERIFY(response.startsWith("HTTP/1.1 200 OK\r\n"));
    QVERIFY(response.toLower().contains("connection: close\r\n"));
    QVERIFY(response.toLower().contains("content-length: 11\r\n"));
    QVERIFY(response.toLower().contains("mcp-session-id: ticket\r\n"));
    QTRY_COMPARE(disconnected.size(), 1);
    QVERIFY(!server.respond(id, 200));
}

void HttpMcpServerTests::fullSizeBodyAndRepeatedSmallChunks()
{
    HttpMcpServer server;
    const int port = listen(server);
    QVERIFY(port > 0);
    QSignalSpy requests(&server, &HttpMcpServer::request);
    const QByteArray body = "{\"text\":\""
        + QByteArray(NovaTerm::Mcp::MaxRequestBytes - 11, 'a') + "\"}";
    QCOMPARE(body.size(), NovaTerm::Mcp::MaxRequestBytes);
    for (bool chunked : {false, true}) {
        QTcpSocket socket;
        socket.connectToHost("127.0.0.1", quint16(port));
        QVERIFY(socket.waitForConnected());
        QByteArray framing;
        QByteArray headers;
        if (chunked) {
            headers = "Transfer-Encoding: chunked\r\n";
            // 大正文用少量块；原始报文仍受请求正文及16KiB framing 总额度约束。
            for (int start = 0; start < body.size(); start += 65536) {
                const auto chunk = body.mid(start, 65536);
                framing += QByteArray::number(chunk.size(), 16) + ";tag=ok\r\n" + chunk + "\r\n";
            }
            framing += "0\r\n\r\n";
        } else {
            headers = "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
            framing = body;
        }
        socket.write(getRequest(port, "POST", headers) + framing); socket.flush();
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 1, 5000);
        QCOMPARE(requests.first().at(1).toJsonObject().value("message").toObject()
            .value("text").toString().size(), NovaTerm::Mcp::MaxRequestBytes - 11);
        QVERIFY(server.respond(requests.takeFirst().at(0).toString(), 202));
        QVERIFY(collect(socket).startsWith("HTTP/1.1 202 "));
    }
}


void HttpMcpServerTests::requestFloodWhileGuiBlockedIsBounded()
{
    HttpMcpServer server;
    const int port = listen(server);
    QVERIFY(port > 0);
    QSignalSpy requests(&server, &HttpMcpServer::request);
    // 仅使用 socket 的阻塞等待；此循环刻意不给 GUI 分发 request/disconnected。
    for (int i = 0; i < 33; ++i) {
        QTcpSocket socket;
        socket.connectToHost("127.0.0.1", quint16(port));
        QVERIFY(socket.waitForConnected());
        socket.write(getRequest(port)); socket.flush();
        if (i < 32) {
            // 等 I/O 真正接纳后再断连；不靠固定 sleep 猜测 worker 的进度。
            QElapsedTimer admitted;
            admitted.start();
            while (server.statistics().value("acceptedRequests").toString().toInt() < i + 1
                && admitted.elapsed() < 2000)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            QCOMPARE(server.statistics().value("acceptedRequests").toString().toInt(), i + 1);
            socket.abort();
        } else {
            const bool ready = socket.bytesAvailable() > 0 || socket.waitForReadyRead(2000);
            QVERIFY2(ready, qPrintable(socket.errorString() + " "
                + QString::fromUtf8(QJsonDocument(server.statistics()).toJson(QJsonDocument::Compact))));
            QVERIFY(socket.readAll().startsWith("HTTP/1.1 503 "));
        }
    }
    QTRY_COMPARE(requests.size(), 32);
}

void HttpMcpServerTests::blockedClientOutputEventuallyRejects()
{
    HttpMcpServer server;
    const int port = listen(server);
    QVERIFY(port > 0);
    QSignalSpy requests(&server, &HttpMcpServer::request);
    QSignalSpy disconnected(&server, &HttpMcpServer::disconnected);
    QTcpSocket socket;
    socket.setReadBufferSize(1);
    socket.connectToHost("127.0.0.1", quint16(port));
    QVERIFY(socket.waitForConnected());
    socket.write(getRequest(port)); socket.flush();
    QTRY_COMPARE(requests.size(), 1);
    const auto id = requests.first().at(0).toString();
    const QByteArray body = "{\"text\":\"" + QByteArray(2 * 1024 * 1024, 'a') + "\"}";
    bool rejected = false;
    for (int i = 0; i < 64 && !rejected; ++i)
        rejected = !server.respond(id, 200, body, {}, true, false);
    QVERIFY(rejected);
    QTRY_COMPARE(disconnected.size(), 1);
    QVERIFY(!server.respond(id, 200, {}, {}, true));
}

void HttpMcpServerTests::rapidStartStopDoesNotNeedGuiEvents()
{
    HttpMcpServer server;
    QElapsedTimer timer; timer.start();
    for (int i = 0; i < 30; ++i) { server.start(); server.stop(); }
    QVERIFY(timer.elapsed() < 5000);
    const int port = listen(server);
    QVERIFY(port > 0);
}

void HttpMcpServerTests::rejectsMalformedHttp_data()
{
    QTest::addColumn<QByteArray>("headers");
    QTest::addColumn<QByteArray>("tail");
    QTest::addColumn<int>("status");
    QTest::newRow("bad-host") << QByteArray("Host: evil.invalid\r\n") << QByteArray() << 400;
    QTest::newRow("bad-port") << QByteArray("Host: localhost:1\r\n") << QByteArray() << 400;
    QTest::newRow("duplicate-host") << QByteArray("Host: @HOST@\r\nHost: @HOST@\r\n") << QByteArray() << 400;
    QTest::newRow("origin-evil") << QByteArray("Host: @HOST@\r\nOrigin: http://evil.invalid\r\n") << QByteArray() << 403;
    QTest::newRow("origin-null") << QByteArray("Host: @HOST@\r\nOrigin: null\r\n") << QByteArray() << 403;
    QTest::newRow("origin-path") << QByteArray("Host: @HOST@\r\nOrigin: http://@HOST@/\r\n") << QByteArray() << 403;
    QTest::newRow("origin-userinfo") << QByteArray("Host: @HOST@\r\nOrigin: http://user@@HOST@\r\n") << QByteArray() << 403;
    QTest::newRow("duplicate-length") << QByteArray("Host: @HOST@\r\nContent-Length: 0\r\ncontent-length: 0\r\n") << QByteArray() << 400;
    QTest::newRow("conflicting-framing") << QByteArray("Host: @HOST@\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n") << QByteArray() << 400;
    QTest::newRow("negative-length") << QByteArray("Host: @HOST@\r\nContent-Length: -1\r\n") << QByteArray() << 400;
    QTest::newRow("overflow-length") << QByteArray("Host: @HOST@\r\nContent-Length: 184467440737095516160\r\n") << QByteArray() << 400;
    const auto oversizedLength = QByteArray::number(NovaTerm::Mcp::MaxRequestBytes + 1);
    QTest::newRow("body-too-large") << QByteArray("Host: @HOST@\r\nContent-Length: ") + oversizedLength + "\r\n" << QByteArray() << 413;
    QTest::newRow("header-too-large") << QByteArray("Host: @HOST@\r\nX-Fill: ") + QByteArray(17000, 'a') + "\r\n" << QByteArray() << 431;
    QTest::newRow("folded-header") << QByteArray("Host: @HOST@\r\n X-Foo: a\r\n") << QByteArray() << 400;
    QTest::newRow("length-list") << QByteArray("Host: @HOST@\r\nContent-Length: 0, 0\r\n") << QByteArray() << 400;
    QTest::newRow("unsupported-coding") << QByteArray("Host: @HOST@\r\nTransfer-Encoding: gzip, chunked\r\n") << QByteArray() << 400;
    QTest::newRow("bad-chunk-size") << QByteArray("Host: @HOST@\r\nTransfer-Encoding: chunked\r\n") << QByteArray("-1\r\n") << 400;
    QTest::newRow("too-large-chunk") << QByteArray("Host: @HOST@\r\nTransfer-Encoding: chunked\r\n") << QByteArray::number(NovaTerm::Mcp::MaxRequestBytes + 1, 16) + "\r\n" << 413;
    QTest::newRow("bad-chunk-crlf") << QByteArray("Host: @HOST@\r\nTransfer-Encoding: chunked\r\n") << QByteArray("2\r\n{}xx") << 400;
    QTest::newRow("framing-trailer") << QByteArray("Host: @HOST@\r\nTransfer-Encoding: chunked\r\n") << QByteArray("0\r\nContent-Length: 0\r\n\r\n") << 400;
    QTest::newRow("unknown-expectation") << QByteArray("Host: @HOST@\r\nContent-Length: 10\r\nExpect: other\r\n") << QByteArray() << 417;
    QTest::newRow("expect-bad-host") << QByteArray("Host: evil.invalid\r\nContent-Length: 10\r\nExpect: 100-continue\r\n") << QByteArray() << 400;
    QTest::newRow("expect-bad-origin") << QByteArray("Host: @HOST@\r\nOrigin: null\r\nContent-Length: 10\r\nExpect: 100-continue\r\n") << QByteArray() << 403;
    QTest::newRow("expect-body-too-large") << QByteArray("Host: @HOST@\r\nContent-Length: ") + oversizedLength + "\r\nExpect: 100-continue\r\n" << QByteArray() << 413;
}

void HttpMcpServerTests::rejectsMalformedHttp()
{
    QFETCH(QByteArray, headers);
    QFETCH(QByteArray, tail);
    QFETCH(int, status);
    HttpMcpServer server;
    const int port = listen(server);
    QVERIFY(port > 0);
    QSignalSpy requests(&server, &HttpMcpServer::request);
    headers.replace("@HOST@", QByteArray("localhost:") + QByteArray::number(port));
    QTcpSocket socket;
    socket.connectToHost("127.0.0.1", quint16(port));
    QVERIFY(socket.waitForConnected());
    socket.write("POST /mcp HTTP/1.1\r\n" + headers + "\r\n" + tail);
    socket.flush();
    const auto response = collect(socket);
    QVERIFY2(response.startsWith("HTTP/1.1 " + QByteArray::number(status) + " "), response.constData());
    QCOMPARE(requests.size(), 0);
}

void HttpMcpServerTests::chunkedPostAndJsonErrors_data()
{
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<int>("parseError");
    QTest::newRow("object") << QByteArray("{\"id\":1}") << 0;
    QTest::newRow("invalid") << QByteArray("{") << -32700;
    QTest::newRow("array") << QByteArray("[]") << -32600;
}

void HttpMcpServerTests::chunkedPostAndJsonErrors()
{
    QFETCH(QByteArray, body);
    QFETCH(int, parseError);
    HttpMcpServer server;
    const int port = listen(server);
    QVERIFY(port > 0);
    QSignalSpy requests(&server, &HttpMcpServer::request);
    QTcpSocket socket;
    socket.connectToHost("127.0.0.1", quint16(port));
    QVERIFY(socket.waitForConnected());
    const auto framing = QByteArray::number(body.size(), 16) + "\r\n" + body + "\r\n0\r\n\r\n";
    socket.write(getRequest(port, "POST", "Transfer-Encoding: chunked\r\nOrigin: http://localhost:"
        + QByteArray::number(port) + "\r\n") + framing.left(2));
    socket.flush();
    QTest::qWait(10);
    QCOMPARE(requests.size(), 0);
    socket.write(framing.mid(2));
    socket.flush();
    QTRY_COMPARE(requests.size(), 1);
    const auto envelope = requests.first().at(1).toJsonObject();
    QCOMPARE(envelope.value("parseError").toInt(), parseError);
    if (!parseError) QCOMPARE(envelope.value("message").toObject().value("id").toInt(), 1);
    QVERIFY(server.respond(requests.first().at(0).toString(), 202));
    QVERIFY(collect(socket).startsWith("HTTP/1.1 202 Accepted\r\n"));
}

void HttpMcpServerTests::sseStreamsChunksAndTerminates()
{
    HttpMcpServer server;
    const int port = listen(server);
    QVERIFY(port > 0);
    QSignalSpy requests(&server, &HttpMcpServer::request);
    QTcpSocket socket;
    socket.connectToHost("127.0.0.1", quint16(port));
    QVERIFY(socket.waitForConnected());
    socket.write(getRequest(port)); socket.flush();
    QTRY_COMPARE(requests.size(), 1);
    const auto id = requests.first().at(0).toString();
    QVERIFY(server.respond(id, 200, {}, {}, true, false));
    QByteArray response;
    QTRY_VERIFY((response += socket.readAll()).contains("\r\n\r\n"));
    QVERIFY(response.toLower().contains("content-type: text/event-stream\r\n"));
    QVERIFY(response.toLower().contains("transfer-encoding: chunked\r\n"));
    QVERIFY(server.respond(id, 200, "{\"id\":1}", {}, true, false));
    QVERIFY(server.respond(id, 200, "{\"id\":2}", {}, true, true));
    response += collect(socket);
    QVERIFY(response.endsWith("f\r\ndata:{\"id\":1}\n\n\r\nf\r\ndata:{\"id\":2}\n\n\r\n0\r\n\r\n"));
    QVERIFY(!server.respond(id, 200, {}, {}, true));
}

void HttpMcpServerTests::emptyResponsesAndRestart()
{
    HttpMcpServer server;
    for (int status : {202, 204}) {
        const int port = listen(server);
        QVERIFY(port > 0);
        QSignalSpy requests(&server, &HttpMcpServer::request);
        QTcpSocket socket;
        socket.connectToHost("127.0.0.1", quint16(port));
        QVERIFY(socket.waitForConnected());
        socket.write(getRequest(port, "DELETE")); socket.flush();
        QTRY_COMPARE(requests.size(), 1);
        QVERIFY(server.respond(requests.first().at(0).toString(), status));
        const auto response = collect(socket);
        QVERIFY(response.startsWith("HTTP/1.1 " + QByteArray::number(status)));
        QVERIFY(response.endsWith("\r\n\r\n"));
        server.stop();
        QVERIFY(!server.respond(requests.first().at(0).toString(), 200));
    }
}

void HttpMcpServerTests::responseCreditAndHeaderInjectionAreRejected()
{
    HttpMcpServer server;
    const int port = listen(server);
    QVERIFY(port > 0);
    QSignalSpy requests(&server, &HttpMcpServer::request);
    QTcpSocket socket;
    socket.connectToHost("127.0.0.1", quint16(port));
    QVERIFY(socket.waitForConnected());
    socket.write(getRequest(port)); socket.flush();
    QTRY_COMPARE(requests.size(), 1);
    const auto id = requests.first().at(0).toString();
    QVERIFY(!server.respond(id, 200, {}, {{"X-Test", "a\r\nInjected: yes"}}));
    QVERIFY(!server.respond(id, 200, {}, {{"Content-Length", "999"}}));
    QVERIFY(!server.respond(id, 200, QByteArray(4 * 1024 * 1024 + 1, 'a')));
    QTRY_COMPARE(socket.state(), QAbstractSocket::UnconnectedState);
}

void HttpMcpServerTests::socketLimitAndStopWithoutGuiRoundtrip()
{
    HttpMcpServer server;
    const int port = listen(server);
    QVERIFY(port > 0);
    std::vector<std::unique_ptr<QTcpSocket>> sockets;
    for (int i = 0; i < 17; ++i) {
        auto socket = std::make_unique<QTcpSocket>();
        socket->connectToHost("127.0.0.1", quint16(port));
        QVERIFY(socket->waitForConnected());
        sockets.push_back(std::move(socket));
    }
    const auto response = collect(*sockets.back());
    QVERIFY(response.isEmpty() || response.startsWith("HTTP/1.1 503 "));
    QCOMPARE(sockets.back()->state(), QAbstractSocket::UnconnectedState);
    QElapsedTimer timer; timer.start();
    server.stop();
    QVERIFY(timer.elapsed() < 3000);
    for (auto& socket : sockets) {
        collect(*socket);
        QCOMPARE(socket->state(), QAbstractSocket::UnconnectedState);
    }
}

QTEST_GUILESS_MAIN(HttpMcpServerTests)
#include "HttpMcpServerTests.moc"
