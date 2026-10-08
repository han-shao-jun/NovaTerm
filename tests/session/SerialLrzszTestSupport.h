/** @file SerialLrzszTestSupport.h @brief 本机 lrzsz 与真实 Session 的虚拟串口验收。 */
#pragma once
#include "session/TerminalSession.h"
#include "session/SessionInputArbiter.h"
#include "session/transfer/SerialFileTransferController.h"
#include "transport/SerialTransport.h"
#include "core/terminal/TerminalCore.h"
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSerialPort>
#include <QSocketNotifier>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <algorithm>
#include <array>
#include <memory>
#include <vector>
#ifdef Q_OS_LINUX
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace SerialLrzszTests {
/** @brief 断言失败也回收对端及 socat，不残留虚拟串口或子进程。 */
class ScopedProcess final : public QProcess {
public:
    ~ScopedProcess() override
    {
        if (state() != QProcess::NotRunning) { kill(); waitForFinished(1000); }
    }
};
#ifdef Q_OS_LINUX
/** @brief 有界透明中继；队列满时停止读 socket，让外部进程自然背压。 */
class VirtualPeerRelay final : public QObject {
public:
    static constexpr qint64 QueueLimit = 256 * 1024;
    static constexpr qint64 ChunkBytes = 64 * 1024;
    VirtualPeerRelay(int descriptor, QSerialPort& port, SerialFileTransferController& controller)
        : _descriptor(descriptor), _port(port), _controller(controller)
        , _readable(descriptor,QSocketNotifier::Read), _writable(descriptor,QSocketNotifier::Write)
    {
        _writable.setEnabled(false);
        connect(&_readable,&QSocketNotifier::activated,this,[this] { readSocket(); });
        connect(&_writable,&QSocketNotifier::activated,this,[this] { flushSocket(); readPort(); });
        connect(&_port,&QSerialPort::readyRead,this,[this] { readPort(); });
        connect(&_port,&QSerialPort::bytesWritten,this,[this](qint64) {
            if (!_readEnded) { _readable.setEnabled(true); readSocket(); }
        });
    }
private:
    void fail(const QString& reason)
    {
        _readEnded = _writeEnded = true;
        _readable.setEnabled(false); _writable.setEnabled(false); _pending.clear();
        _controller.abort(reason);
    }
    void readSocket()
    {
        std::array<char,ChunkBytes> bytes{};
        while (!_readEnded) {
            const auto available = QueueLimit - _port.bytesToWrite();
            if (available <= 0) { _readable.setEnabled(false); return; }
            const auto count = ::recv(_descriptor,bytes.data(),std::size_t(std::min(ChunkBytes,available)),0);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            // 对端退出且有未读反向字节时，socket 可在真实 ACK 之后
            // 返回 reset；已排入串口的 ACK 仍必须继续排空。
            if (count < 0 && errno == ECONNRESET) {
                _readEnded = true; _readable.setEnabled(false); return;
            }
            if (count < 0) { fail(QStringLiteral("Virtual peer socket read failed")); return; }
            if (count == 0) { _readEnded = true; _readable.setEnabled(false); return; }
            if (_port.write(bytes.data(),count) != count) {
                fail(QStringLiteral("Virtual peer serial write failed")); return;
            }
        }
    }
    void flushSocket()
    {
        while (!_pending.isEmpty() && !_writeEnded) {
            const auto count = ::send(_descriptor,_pending.constData(),
                std::size_t(std::min<qint64>(ChunkBytes,_pending.size())),MSG_NOSIGNAL);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                _writable.setEnabled(true); return;
            }
            // 对端退出后不再消费应答；仍继续读取 socket 中真实的最后 ACK。
            if (count < 0 && errno == EPIPE) { _writeEnded = true; _pending.clear(); break; }
            if (count <= 0) { fail(QStringLiteral("Virtual peer socket write failed (errno %1)").arg(errno)); return; }
            _pending.remove(0,count);
        }
        _writable.setEnabled(false);
    }
    void readPort()
    {
        flushSocket();
        if (_writeEnded) { _port.readAll(); return; }
        while (_port.bytesAvailable() > 0) {
            const auto available = QueueLimit - _pending.size();
            if (available <= 0) return;
            const auto bytes = _port.read(std::min(ChunkBytes,available));
            if (bytes.isEmpty()) return;
            _pending.append(bytes); flushSocket();
            if (_writeEnded) return;
        }
    }
    int _descriptor;
    QSerialPort& _port;
    SerialFileTransferController& _controller;
    QSocketNotifier _readable, _writable;
    QByteArray _pending;
    bool _readEnded{false}, _writeEnded{false};
};
#endif
inline void data()
{
    QTest::addColumn<int>("protocol");
    QTest::addColumn<quint64>("size");
    QTest::addColumn<bool>("sending");
    if (qEnvironmentVariable("NOVATERM_SERIAL_LRZSZ_TESTS") != QStringLiteral("1")) {
        QTest::newRow("manual-only") << 4 << quint64(0) << true;
        return;
    }
    for (int protocol = 0; protocol < 5; ++protocol) {
        std::vector<quint64> sizes{0,1,127,128,129,1023,1024,1025,8191,8192,8193,
                                  64 * 1024,1024 * 1024,5 * 1024 * 1024,10 * 1024 * 1024};
        if (protocol < 4) {
            const quint64 block = protocol < 2 ? 128 : 1024;
            for (const quint64 packets : {255,256,257})
                for (int delta = -1; delta <= 1; ++delta)
                    sizes.push_back(packets * block - 1 + quint64(delta + 1));
        }
        std::sort(sizes.begin(),sizes.end());
        sizes.erase(std::unique(sizes.begin(),sizes.end()),sizes.end());
        for (const auto size : sizes) for (const bool sending : {true,false}) {
            const auto row = QStringLiteral("%1-%2-%3").arg(protocol)
                .arg(sending ? QStringLiteral("send") : QStringLiteral("receive")).arg(size);
            QTest::newRow(qPrintable(row)) << protocol << size << sending;
        }
    }
}
inline void run()
{
#ifdef Q_OS_LINUX
    if (qEnvironmentVariable("NOVATERM_SERIAL_LRZSZ_TESTS") != QStringLiteral("1"))
        QSKIP("Set NOVATERM_SERIAL_LRZSZ_TESTS=1 for the manual virtual serial/lrzsz matrix");
    QFETCH(int,protocol); QFETCH(quint64,size); QFETCH(bool,sending);
    const auto socat = QStandardPaths::findExecutable(QStringLiteral("socat"));
    const auto rz = QStandardPaths::findExecutable(QStringLiteral("rz"));
    const auto sz = QStandardPaths::findExecutable(QStringLiteral("sz"));
    QVERIFY2(!socat.isEmpty() && !rz.isEmpty() && !sz.isEmpty(), "socat, rz and sz must be installed");
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto sourceDirectory = directory.filePath(QStringLiteral("source"));
    const auto targetDirectory = directory.filePath(QStringLiteral("target"));
    QVERIFY(QDir().mkpath(sourceDirectory)); QVERIFY(QDir().mkpath(targetDirectory));
    const auto sourcePath = sourceDirectory + QStringLiteral("/serial-size.bin");
    const auto targetPath = targetDirectory + QStringLiteral("/serial-size.bin");
    QByteArray contents(qsizetype(size),Qt::Uninitialized);
    for (qsizetype index = 0; index < contents.size(); ++index)
        contents[index] = char((quint64(index) * 73U + quint64(index) / 251U) & 0xffU);
    if (contents.size() >= 2) { contents[contents.size()-2] = char(0x1a); contents.back() = 0; }
    QFile source(sourcePath); QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(contents), qint64(contents.size())); source.close();
    const auto clientPort = directory.filePath(QStringLiteral("client"));
    const auto peerPort = directory.filePath(QStringLiteral("peer"));
    ScopedProcess bridge;
    bridge.start(socat, {QStringLiteral("pty,raw,echo=0,link=") + clientPort,
                         QStringLiteral("pty,raw,echo=0,link=") + peerPort});
    QVERIFY(bridge.waitForStarted(1000));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(clientPort) && QFileInfo::exists(peerPort),3000);
    // lrzsz 的 TCIOFLUSH 会丢弃尚未被 master 读取的最后 ACK；PTY
    // 的 tcdrain 不保证这段字节已被桥接端消费。默认用透明字节中继，
    // 将 lrzsz 的 socket 与真实虚拟串口对分开，仍要求收到真实协议 ACK。
    const bool directPeerPty = qEnvironmentVariable("NOVATERM_SERIAL_LRZSZ_DIRECT_PTY") == QStringLiteral("1");
    QFile peerDevice;
    QFile socketOwner;
    QSerialPort relayPort;
    ScopedProcess peer;
    peer.setWorkingDirectory(targetDirectory);
    if (directPeerPty) {
        // 保留直接 PTY 模式供复现；O_NOCTTY 避免父测试在关端口时收到 HUP。
        const int descriptor = ::open(QFile::encodeName(peerPort).constData(),O_RDWR | O_NOCTTY);
        QVERIFY(descriptor >= 0);
        QVERIFY(peerDevice.open(descriptor,QIODevice::ReadWrite,QFileDevice::AutoCloseHandle));
    } else {
        int descriptors[2];
        QVERIFY(::socketpair(AF_UNIX,SOCK_STREAM | SOCK_CLOEXEC,0,descriptors) == 0);
        QVERIFY(socketOwner.open(descriptors[0],QIODevice::ReadWrite,QFileDevice::AutoCloseHandle));
        QVERIFY(peerDevice.open(descriptors[1],QIODevice::ReadWrite,QFileDevice::AutoCloseHandle));
        const int flags = ::fcntl(descriptors[0],F_GETFL);
        QVERIFY(flags >= 0 && ::fcntl(descriptors[0],F_SETFL,flags | O_NONBLOCK) == 0);
        relayPort.setPortName(peerPort);
        QVERIFY(relayPort.setBaudRate(115200));
        relayPort.setReadBufferSize(VirtualPeerRelay::QueueLimit);
        QVERIFY2(relayPort.open(QIODevice::ReadWrite),qPrintable(relayPort.errorString()));
    }
    const int descriptor = int(peerDevice.handle());
    peer.setChildProcessModifier([descriptor,&peer] {
        if (::dup2(descriptor,STDIN_FILENO) < 0 || ::dup2(descriptor,STDOUT_FILENO) < 0)
            peer.failChildProcessModifier("dup2 virtual peer",errno);
        if (descriptor > STDERR_FILENO) ::close(descriptor);
    });
    QStringList arguments{QStringLiteral("-b")};
    if (protocol < 3) arguments.append(QStringLiteral("--xmodem"));
    if (protocol == 3) arguments.append(QStringLiteral("--ymodem"));
    if (sending && protocol < 3) {
        arguments.append(targetPath);
        if (protocol != 0) arguments.append(QStringLiteral("-c"));
    }
    if (!sending) {
        if (protocol == 2) arguments.append(QStringLiteral("-k"));
        arguments.append(sourcePath);
    }
    TerminalCore core(80,24);
    TerminalSession session(&core);
    SerialConfig serialConfig; serialConfig.portName = clientPort; serialConfig.baudRate = 115200;
    session.attach(new SerialTransport(serialConfig),TerminalSession::Ownership::Adopt,TransportKind::Serial);
    QVERIFY(session.start()); QTRY_VERIFY(session.state() == SessionState::Running);
    auto* controller = session.serialFileTransfer(); QVERIFY(controller);
    QByteArray inboundTail; QObject traceContext;
    std::unique_ptr<VirtualPeerRelay> relay;
    if (!directPeerPty) relay = std::make_unique<VirtualPeerRelay>(int(socketOwner.handle()),relayPort,*controller);
    QObject::connect(session.transport(),&ITransport::readyRead,&traceContext,[&](const QByteArray& bytes) {
        inboundTail.append(bytes.last(std::min<qsizetype>(bytes.size(),128)));
        if (inboundTail.size() > 128) inboundTail.remove(0,inboundTail.size()-128);
    });
    QSignalSpy finished(controller,&SerialFileTransferController::finished);
    QSignalSpy disconnected(&session,&TerminalSession::disconnected);
    const auto generation = session.statistics().generation;
    SerialTransferRequest request; request.protocol = SerialTransferProtocol(protocol);
    request.direction = sending ? NovaTerm::FileTransfer::Direction::Send
                                : NovaTerm::FileTransfer::Direction::Receive;
    if (sending) request.files = {sourcePath};
    else {
        request.destination = protocol < 3 ? targetPath : targetDirectory;
        if (protocol < 3) request.expectedSize = size;
    }
    QVERIFY2(controller->start(request),qPrintable(controller->errorString()));
    // 连续事件循环驱动跨线程磁盘结果与 QSerialPort；QTRY 的轮询间隔
    // 会在每个停止等待包之间插入延迟，不能用它测大文件的联通通路。
    QEventLoop transferLoop; QTimer deadline, peerDrain;
    deadline.setSingleShot(true); peerDrain.setSingleShot(true);
    QObject::connect(controller,&SerialFileTransferController::finished,&transferLoop,&QEventLoop::quit);
    QObject::connect(&deadline,&QTimer::timeout,&transferLoop,&QEventLoop::quit);
    QObject::connect(&peerDrain,&QTimer::timeout,&transferLoop,&QEventLoop::quit);
    // 对端退出后允许默认 3 秒 Closing 窗口和 1 秒链路排空；不能
    // 无限重试。若最后确认丢失，保留失败而不是伪造 ACK。
    constexpr int PeerExitGraceMs = 4000;
    QObject::connect(&peer,&QProcess::finished,&transferLoop,[&] {
        if (controller->isActive()) peerDrain.start(PeerExitGraceMs);
    });
    peer.start(sending ? rz : sz,arguments); QVERIFY(peer.waitForStarted(1000));
    if (!directPeerPty) peerDevice.close();
    deadline.start(30000);
    if (peer.state() == QProcess::NotRunning && controller->isActive()) peerDrain.start(PeerExitGraceMs);
    if (controller->isActive()) transferLoop.exec();
    deadline.stop(); peerDrain.stop();
    const auto peerError = QString::fromLocal8Bit(peer.readAllStandardError());
    QFile snapshot(targetPath);
    const bool targetAvailable = snapshot.open(QIODevice::ReadOnly);
    const auto targetBytes = targetAvailable ? snapshot.readAll() : QByteArray{};
    const bool prefixMatches = targetAvailable && targetBytes.size() >= contents.size()
        && QCryptographicHash::hash(targetBytes.first(contents.size()),QCryptographicHash::Sha256)
           == QCryptographicHash::hash(contents,QCryptographicHash::Sha256);
    snapshot.close();
    const auto diagnostic = QStringLiteral("Virtual serial incomplete at %1 bytes; status=%2; peerState=%3; peerExit=%4; targetBytes=%5; sourcePrefixMatches=%6; inboundTail=%7; %8")
        .arg(controller->progress().transferredBytes).arg(controller->progress().status)
        .arg(int(peer.state())).arg(peer.exitCode()).arg(targetBytes.size()).arg(prefixMatches)
        .arg(QString::fromLatin1(inboundTail.toHex())).arg(peerError.right(2000));
    QVERIFY2(!controller->isActive(),qPrintable(diagnostic));
    QCOMPARE(finished.size(),1);
    QVERIFY2(finished.front().front().toBool(),qPrintable(finished.front().at(1).toString() + peerError));
    QTRY_COMPARE_WITH_TIMEOUT(peer.state(),QProcess::NotRunning,5000);
    QCOMPARE(peer.exitStatus(),QProcess::NormalExit); QCOMPARE(peer.exitCode(),0);
    QFile received(targetPath); QVERIFY(received.open(QIODevice::ReadOnly));
    const auto actual = received.readAll();
    if (sending && protocol < 3) {
        const quint64 block = protocol == 2 ? 1024 : 128;
        const auto paddedSize = ((size + block - 1) / block) * block;
        QCOMPARE(quint64(actual.size()),paddedSize);
        QCOMPARE(actual.first(contents.size()),contents);
        QCOMPARE(actual.sliced(contents.size()),QByteArray(qsizetype(paddedSize-size),char(0x1a)));
    } else {
        QCOMPARE(actual.size(),contents.size());
        QCOMPARE(QCryptographicHash::hash(actual,QCryptographicHash::Sha256),
                 QCryptographicHash::hash(contents,QCryptographicHash::Sha256));
    }
    QCOMPARE(controller->progress().transferredBytes,size);
    QCOMPARE(controller->progress().completedFiles,quint64(1));
    QCOMPARE(disconnected.size(),0); QCOMPARE(session.state(),SessionState::Running);
    QCOMPARE(session.statistics().generation,generation);
    QVERIFY(session.transport()->isConnected()); QVERIFY(!session.inputArbiter()->hasTransferLease());
    QVERIFY(core.waitForIdle());
    session.close();
#else
    QSKIP("Linux virtual serial fixture with installed lrzsz and socat");
#endif
}
} // namespace SerialLrzszTests
