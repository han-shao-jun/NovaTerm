/**
 * @file   TelnetTransportTests.cpp
 * @brief  TelnetTransport 协议与失败路径测试。
 *
 * 用 loopback QTcpServer 充当极简 telnet 服务端，无需真实 telnetd 即可
 * 端到端验证：客户端主动协商、NAWS 子协商报文、TERMINAL-TYPE 应答、
 * 双向 IAC 转义、背压，以及连接失败路径的错误上报与资源回收。
 */
#include "session/SessionTypes.h"
#include "transport/ITransport.h"
#include "transport/TelnetTransport.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

#include <initializer_list>

namespace {

// ── Telnet 协议常量。测试侧独立定义，不依赖 libtelnet.h（PImpl 已将其
//    隔离在 TelnetTransport.cpp 内），顺带交叉校验实现用的编码正确。──
enum : int {
    IAC = 0xFF,
    SE = 0xF0,
    NOP = 0xF1,
    SB = 0xFA,
    WILL = 0xFB,
    WONT = 0xFC,
    DO = 0xFD,
    DONT = 0xFE,
    OPT_BINARY = 0,
    OPT_ECHO = 1,
    OPT_SGA = 3,
    OPT_TTYPE = 24,
    OPT_NAWS = 31,
    TTYPE_IS = 0,
    TTYPE_SEND = 1,
};

/// 按字节值构造 QByteArray，避免 char 符号性与 QByteArray 构造重载歧义。
QByteArray bytes(std::initializer_list<int> values)
{
    QByteArray out;
    out.reserve(static_cast<qsizetype>(values.size()));
    for (const int value : values)
        out.append(static_cast<char>(static_cast<unsigned char>(value)));
    return out;
}

TelnetConfig makeConfig(quint16 port, bool naws = true, bool binary = false)
{
    TelnetConfig config;
    config.host = QStringLiteral("127.0.0.1");
    config.port = port;
    config.terminalType = QStringLiteral("xterm-256color");
    config.naws = naws;
    config.binaryMode = binary;
    config.keepAliveSeconds = 0;
    return config;
}

/// 极简 telnet 服务端：接受一条连接，累积收到的字节，可主动回发。
class LoopbackServer final : public QObject
{
    Q_OBJECT
public:
    bool listen()
    {
        connect(&_server, &QTcpServer::newConnection, this, [this]() {
            _peer = _server.nextPendingConnection();
            connect(_peer, &QTcpSocket::readyRead, this, [this]() {
                _received.append(_peer->readAll());
            });
        });
        return _server.listen(QHostAddress::LocalHost, 0);
    }

    [[nodiscard]] quint16 port() const { return _server.serverPort(); }
    [[nodiscard]] const QByteArray& received() const { return _received; }
    void clearReceived() { _received.clear(); }

    void send(const QByteArray& data)
    {
        if (_peer != nullptr) {
            _peer->write(data);
            _peer->flush();
        }
    }

    void closePeer()
    {
        if (_peer != nullptr)
            _peer->disconnectFromHost();
    }

    bool waitForPeer(int timeoutMs = 3000)
    {
        return waitFor([this]() { return _peer != nullptr; }, timeoutMs);
    }

    /// 等待累计收到的字节中出现 needle。
    bool waitForBytes(const QByteArray& needle, int timeoutMs = 3000)
    {
        return waitFor([this, &needle]() { return _received.contains(needle); },
                       timeoutMs);
    }

    template <typename Predicate>
    static bool waitFor(Predicate predicate, int timeoutMs = 3000)
    {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < timeoutMs)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        return predicate();
    }

private:
    QTcpServer _server;
    QTcpSocket* _peer{nullptr};
    QByteArray _received;
};

} // namespace

class TelnetTransportTests final : public QObject
{
    Q_OBJECT
private slots:
    // ── 配置校验 ────────────────────────────────────────────────
    void invalidConfigFailsSynchronously()
    {
        TelnetConfig config;   // host 为空
        TelnetTransport transport(config);
        QSignalSpy errors(&transport, &ITransport::errorOccurred);

        QVERIFY(!transport.connectToHost());
        QCOMPARE(errors.count(), 1);
        QVERIFY(!transport.errorString().isEmpty());
        QVERIFY(!transport.isConnected());
    }

    void zeroPortIsRejected()
    {
        const TelnetConfig config = makeConfig(0);
        QVERIFY(!config.isValid());
        TelnetTransport transport(config);
        QVERIFY(!transport.connectToHost());
    }

    void capabilitiesAdvertiseResizeAndKeepAlive()
    {
        TelnetTransport transport(makeConfig(23));
        const TransportCapabilities caps = transport.capabilities();
        QVERIFY(caps.testFlag(TransportCapability::PauseReads));
        QVERIFY(caps.testFlag(TransportCapability::ResizeTerminal));
        QVERIFY(caps.testFlag(TransportCapability::KeepAlive));
        QVERIFY(caps.testFlag(TransportCapability::Reconnect));
    }

    // ── 主动协商：填 telopt 表不发起协商，必须显式 telnet_negotiate() ──
    void clientInitiatesNegotiationOnConnect()
    {
        LoopbackServer server;
        QVERIFY(server.listen());

        TelnetTransport transport(makeConfig(server.port()));
        QSignalSpy connected(&transport, &ITransport::connected);
        QVERIFY(transport.connectToHost());
        QVERIFY(connected.wait(3000));
        QVERIFY(server.waitForPeer());

        // 字符模式关键三项 + 终端类型 + 窗口尺寸意向。
        QVERIFY(server.waitForBytes(bytes({IAC, DO, OPT_SGA})));
        QVERIFY(server.received().contains(bytes({IAC, WILL, OPT_SGA})));
        QVERIFY(server.received().contains(bytes({IAC, DO, OPT_ECHO})));
        QVERIFY(server.received().contains(bytes({IAC, WILL, OPT_TTYPE})));
        QVERIFY(server.received().contains(bytes({IAC, WILL, OPT_NAWS})));
        // 未开启 binaryMode 时不应申报 BINARY。
        QVERIFY(!server.received().contains(bytes({IAC, WILL, OPT_BINARY})));
    }

    void binaryModeIsAdvertisedOnlyWhenEnabled()
    {
        LoopbackServer server;
        QVERIFY(server.listen());

        TelnetTransport transport(
            makeConfig(server.port(), /*naws=*/false, /*binary=*/true));
        QSignalSpy connected(&transport, &ITransport::connected);
        QVERIFY(transport.connectToHost());
        QVERIFY(connected.wait(3000));
        QVERIFY(server.waitForPeer());

        QVERIFY(server.waitForBytes(bytes({IAC, WILL, OPT_BINARY})));
        QVERIFY(server.received().contains(bytes({IAC, DO, OPT_BINARY})));
        QVERIFY(!server.received().contains(bytes({IAC, WILL, OPT_NAWS})));
    }

    // ── NAWS 子协商 ─────────────────────────────────────────────
    void nawsIsSentAfterServerAccepts()
    {
        LoopbackServer server;
        QVERIFY(server.listen());

        TelnetTransport transport(makeConfig(server.port()));
        QSignalSpy connected(&transport, &ITransport::connected);
        QVERIFY(transport.connectToHost());
        QVERIFY(connected.wait(3000));
        QVERIFY(server.waitForPeer());
        QVERIFY(server.waitForBytes(bytes({IAC, WILL, OPT_NAWS})));

        // 协商完成前设置的尺寸应被缓存，而非丢弃。
        transport.resizeTerminal(120, 40);
        QVERIFY(!transport.nawsNegotiated());
        server.clearReceived();

        // 服务端接受 → 客户端立即补报缓存尺寸。
        server.send(bytes({IAC, DO, OPT_NAWS}));
        QVERIFY(server.waitForBytes(
            bytes({IAC, SB, OPT_NAWS, 0x00, 120, 0x00, 40, IAC, SE})));
        QVERIFY(transport.nawsNegotiated());

        // 后续 resize 直接发送。
        server.clearReceived();
        transport.resizeTerminal(80, 24);
        QVERIFY(server.waitForBytes(
            bytes({IAC, SB, OPT_NAWS, 0x00, 80, 0x00, 24, IAC, SE})));
    }

    void nawsPayloadEscapesByte255()
    {
        LoopbackServer server;
        QVERIFY(server.listen());

        TelnetTransport transport(makeConfig(server.port()));
        QSignalSpy connected(&transport, &ITransport::connected);
        QVERIFY(transport.connectToHost());
        QVERIFY(connected.wait(3000));
        QVERIFY(server.waitForPeer());

        // 先给出尺寸再完成协商：尺寸未知时实现刻意不发 NAWS 报文。
        transport.resizeTerminal(80, 24);
        server.send(bytes({IAC, DO, OPT_NAWS}));
        QVERIFY(LoopbackServer::waitFor(
            [&transport]() { return transport.nawsNegotiated(); }));

        server.clearReceived();
        // 255 列 → 低字节为 0xFF，SB 载荷内必须加倍，否则对端会把它当 IAC。
        transport.resizeTerminal(255, 24);
        QVERIFY(server.waitForBytes(
            bytes({IAC, SB, OPT_NAWS, 0x00, 0xFF, 0xFF, 0x00, 24, IAC, SE})));
    }

    void nawsIsNotSentWhenServerRefuses()
    {
        LoopbackServer server;
        QVERIFY(server.listen());

        TelnetTransport transport(makeConfig(server.port()));
        QSignalSpy connected(&transport, &ITransport::connected);
        QVERIFY(transport.connectToHost());
        QVERIFY(connected.wait(3000));
        QVERIFY(server.waitForPeer());
        QVERIFY(server.waitForBytes(bytes({IAC, WILL, OPT_NAWS})));

        server.send(bytes({IAC, DONT, OPT_NAWS}));
        QTest::qWait(200);
        server.clearReceived();

        transport.resizeTerminal(100, 30);
        QTest::qWait(200);
        QVERIFY(!transport.nawsNegotiated());
        // 不伪装成功：对端拒绝后不得发送 NAWS 子协商。
        QVERIFY(!server.received().contains(bytes({IAC, SB, OPT_NAWS})));
    }

    // ── TERMINAL-TYPE ──────────────────────────────────────────
    void terminalTypeIsReportedOnRequest()
    {
        LoopbackServer server;
        QVERIFY(server.listen());

        TelnetTransport transport(makeConfig(server.port()));
        QSignalSpy connected(&transport, &ITransport::connected);
        QVERIFY(transport.connectToHost());
        QVERIFY(connected.wait(3000));
        QVERIFY(server.waitForPeer());
        QVERIFY(server.waitForBytes(bytes({IAC, WILL, OPT_TTYPE})));

        server.clearReceived();
        server.send(bytes({IAC, DO, OPT_TTYPE}));
        server.send(bytes({IAC, SB, OPT_TTYPE, TTYPE_SEND, IAC, SE}));

        const QByteArray expected = bytes({IAC, SB, OPT_TTYPE, TTYPE_IS})
            + QByteArrayLiteral("xterm-256color") + bytes({IAC, SE});
        QVERIFY(server.waitForBytes(expected));
    }

    // ── 数据通路与 IAC 转义 ─────────────────────────────────────
    void inboundDataStripsProtocolBytes()
    {
        LoopbackServer server;
        QVERIFY(server.listen());

        TelnetTransport transport(makeConfig(server.port()));
        QByteArray inbound;
        connect(&transport, &ITransport::readyRead, &transport,
                [&inbound](const QByteArray& data) { inbound.append(data); });
        QSignalSpy connected(&transport, &ITransport::connected);
        QVERIFY(transport.connectToHost());
        QVERIFY(connected.wait(3000));
        QVERIFY(server.waitForPeer());

        // 数据与协商混在同一 chunk 内。
        server.send(QByteArrayLiteral("abc") + bytes({IAC, DO, OPT_SGA})
                    + QByteArrayLiteral("def") + bytes({IAC, IAC})
                    + QByteArrayLiteral("ghi"));

        const QByteArray expected =
            QByteArrayLiteral("abcdef") + bytes({IAC}) + QByteArrayLiteral("ghi");
        QVERIFY(LoopbackServer::waitFor(
            [&inbound, &expected]() { return inbound.size() >= expected.size(); }));
        // 协商字节被剥离，0xFF 0xFF 还原为单个 0xFF。
        QCOMPARE(inbound, expected);
    }

    void outboundDataDoublesByte255()
    {
        LoopbackServer server;
        QVERIFY(server.listen());

        TelnetTransport transport(
            makeConfig(server.port(), /*naws=*/false, /*binary=*/true));
        QSignalSpy connected(&transport, &ITransport::connected);
        QVERIFY(transport.connectToHost());
        QVERIFY(connected.wait(3000));
        QVERIFY(server.waitForPeer());

        // 进入 BINARY 后不再做 NVT 换行转换，便于精确断言。
        server.send(bytes({IAC, DO, OPT_BINARY}));
        server.send(bytes({IAC, WILL, OPT_BINARY}));
        QVERIFY(LoopbackServer::waitFor(
            [&transport]() { return transport.binaryNegotiated(); }));
        server.clearReceived();

        transport.write(QByteArrayLiteral("x") + bytes({IAC})
                        + QByteArrayLiteral("y"));
        QVERIFY(server.waitForBytes(QByteArrayLiteral("x") + bytes({IAC, IAC})
                                    + QByteArrayLiteral("y")));
    }

    void carriageReturnBecomesCrNulInNvtMode()
    {
        LoopbackServer server;
        QVERIFY(server.listen());

        TelnetTransport transport(makeConfig(server.port()));
        QSignalSpy connected(&transport, &ITransport::connected);
        QVERIFY(transport.connectToHost());
        QVERIFY(connected.wait(3000));
        QVERIFY(server.waitForPeer());
        QTest::qWait(200);
        server.clearReceived();

        // 未协商 BINARY：RFC 854 要求裸 CR 以 CR NUL 发送。
        transport.write(QByteArrayLiteral("ls\r"));
        QVERIFY(server.waitForBytes(QByteArrayLiteral("ls") + bytes({'\r', 0x00})));
    }

    // ── 背压 ────────────────────────────────────────────────────
    void pausedReadsAreDeferredNotDropped()
    {
        LoopbackServer server;
        QVERIFY(server.listen());

        TelnetTransport transport(makeConfig(server.port()));
        QByteArray inbound;
        connect(&transport, &ITransport::readyRead, &transport,
                [&inbound](const QByteArray& data) { inbound.append(data); });
        QSignalSpy connected(&transport, &ITransport::connected);
        QVERIFY(transport.connectToHost());
        QVERIFY(connected.wait(3000));
        QVERIFY(server.waitForPeer());

        QVERIFY(transport.setReadPaused(true));
        server.send(QByteArrayLiteral("held"));
        QTest::qWait(300);
        QVERIFY2(inbound.isEmpty(), "paused transport must not deliver data");

        // 恢复后数据完整送达，不丢字节。
        transport.setReadPaused(false);
        QVERIFY(LoopbackServer::waitFor(
            [&inbound]() { return inbound.size() >= 4; }));
        QCOMPARE(inbound, QByteArrayLiteral("held"));
    }

    // ── 失败路径与生命周期 ─────────────────────────────────────
    void refusedConnectionReportsRetryableError()
    {
        // 端口 1 上无监听者：连接应被拒绝、上报可重试错误且不崩溃。
        TelnetTransport transport(makeConfig(1));
        QSignalSpy errors(&transport, &ITransport::errorOccurred);
        QSignalSpy structured(&transport, &ITransport::transportError);

        QVERIFY(transport.connectToHost());
        QVERIFY(errors.wait(5000));
        QVERIFY(!transport.isConnected());
        QVERIFY(!transport.errorString().isEmpty());

        QVERIFY(!structured.isEmpty());
        const auto error =
            qvariant_cast<TransportError>(structured.first().first());
        QCOMPARE(error.category, TransportErrorCategory::Connection);
        QVERIFY(error.retryable);
    }

    void remoteCloseEmitsDisconnectedWithoutError()
    {
        LoopbackServer server;
        QVERIFY(server.listen());

        TelnetTransport transport(makeConfig(server.port()));
        QSignalSpy connected(&transport, &ITransport::connected);
        QSignalSpy disconnected(&transport, &ITransport::disconnected);
        QSignalSpy errors(&transport, &ITransport::errorOccurred);
        QVERIFY(transport.connectToHost());
        QVERIFY(connected.wait(3000));
        QVERIFY(server.waitForPeer());

        server.closePeer();
        QVERIFY(disconnected.wait(3000));
        QCOMPARE(disconnected.count(), 1);
        // 对端正常关闭不应上报为错误。
        QCOMPARE(errors.count(), 0);
    }

    void writeBeforeConnectIsRejected()
    {
        TelnetTransport transport(makeConfig(23));
        QSignalSpy errors(&transport, &ITransport::errorOccurred);
        transport.write(QByteArrayLiteral("data"));
        QCOMPARE(errors.count(), 1);
    }

    void reconnectAfterDisconnectSucceeds()
    {
        LoopbackServer server;
        QVERIFY(server.listen());

        TelnetTransport transport(makeConfig(server.port()));
        QSignalSpy connected(&transport, &ITransport::connected);
        QVERIFY(transport.connectToHost());
        QVERIFY(connected.wait(3000));

        transport.disconnect();
        QVERIFY(!transport.isConnected());

        // 复用同一 transport 重连：状态机应被重建，协商重新发起。
        server.clearReceived();
        QVERIFY(transport.connectToHost());
        QVERIFY(connected.wait(3000));
        QCOMPARE(connected.count(), 2);
        QVERIFY(server.waitForBytes(bytes({IAC, WILL, OPT_TTYPE})));
    }
};

QTEST_GUILESS_MAIN(TelnetTransportTests)
#include "TelnetTransportTests.moc"
