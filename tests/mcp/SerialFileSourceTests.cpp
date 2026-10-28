#include "mcp/files/LocalFileAccess.h"
#include "session/transfer/SerialFileTransferController.h"
#include "filetransfer/ZmodemEngine.h"
#include <QElapsedTimer>
#include <QFile>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <atomic>

namespace FT = NovaTerm::FileTransfer;
using NovaTerm::Mcp::Files::FileIoError;
using NovaTerm::Mcp::Files::LocalFileReader;
namespace {
struct SourceTrace {
    std::atomic_int opened{0}, destroyed{0}, reads{0};
    std::atomic_bool workerOnly{true};
    QThread* gui{QThread::currentThread()};
    void checkThread() { if (QThread::currentThread() == gui) workerOnly = false; }
};
class ReaderSource final : public SerialTransferSource {
public:
    ReaderSource(std::unique_ptr<LocalFileReader> reader, std::shared_ptr<SourceTrace> trace)
        : _reader(std::move(reader)), _trace(std::move(trace)) {}
    ~ReaderSource() override { _trace->checkThread(); _reader.reset(); ++_trace->destroyed; }
    quint64 size() const noexcept override { return _reader->size(); }
    bool readAt(quint64 offset, qsizetype count, QByteArray& bytes, QString& error) override
    {
        _trace->checkThread(); ++_trace->reads;
        auto result = _reader->readAt(offset, count);
        if (std::holds_alternative<FileIoError>(result)) { error = "Safe source read refused"; return false; }
        bytes = std::move(std::get<QByteArray>(result));
        return true;
    }
private:
    std::unique_ptr<LocalFileReader> _reader;
    std::shared_ptr<SourceTrace> _trace;
};
SerialTransferSourceSpec sourceSpec(const QString& root, const QString& relative,
    const std::shared_ptr<SourceTrace>& trace)
{
    return {QStringLiteral("artifact.bin"), [root, relative, trace](QString& error) -> std::unique_ptr<SerialTransferSource> {
        trace->checkThread(); ++trace->opened;
        auto opened = LocalFileReader::open(root, relative);
        if (std::holds_alternative<FileIoError>(opened)) { error = "Safe source open refused"; return {}; }
        return std::make_unique<ReaderSource>(std::move(std::get<std::unique_ptr<LocalFileReader>>(opened)), trace);
    }};
}
}
class SerialFileSourceTests : public QObject {
    Q_OBJECT
private slots:
    void safeReaderFeedsRealZmodem_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("binary-file") << 0;
        QTest::newRow("empty-file") << 1;
        QTest::newRow("peer-skips-file") << 2;
    }
    void safeReaderFeedsRealZmodem()
    {
#ifndef Q_OS_WIN
        QSKIP("Native safe local Reader is currently Windows-only.");
#else
        QFETCH(int, mode);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QByteArray contents(mode == 1 ? 0 : 16387, Qt::Uninitialized);
        for (qsizetype i = 0; i < contents.size(); ++i) contents[i] = char(i % 256);
        const auto path = directory.filePath("source.bin");
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(contents), contents.size()); }
        QByteArray transport, peerInput, received;
        bool reserved = false, activated = false;
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; };
        channel.reserve = [&](quint64) { reserved = true; return true; };
        channel.activate = [&] { activated = true; };
        channel.release = [&] { reserved = false; };
        channel.coreIdle = [] { return true; };
        channel.pendingWriteBytes = [&] { return transport.size(); };
        channel.clearWrites = [&] { transport.clear(); return true; };
        channel.write = [&](QByteArrayView bytes) -> qint64 {
            const auto count = qMin<qsizetype>(113, bytes.size()); transport.append(bytes.first(count)); return count;
        };
        SerialFileTransferController controller(channel);
        QSignalSpy done(&controller, &SerialFileTransferController::finished);
        const auto trace = std::make_shared<SourceTrace>();
        SerialTransferRequest request; request.source = sourceSpec(directory.path(), "source.bin", trace);
        QVERIFY(controller.start(request));
        QTRY_COMPARE(trace->opened.load(), 1);
        QTRY_VERIFY(activated);
        QVERIFY(!QFile::rename(path, path + ".moved"));
        FT::ZmodemEngine peer;
        FT::TransferRequest incoming; incoming.direction = FT::Direction::Receive;
        QVERIFY(peer.start(incoming, 0));
        QElapsedTimer elapsed; elapsed.start(); QTimer bridge; bridge.setInterval(1);
        connect(&bridge, &QTimer::timeout, &controller, [&] {
            if (!controller.isActive()) return;
            const auto now = FT::TimePoint(elapsed.elapsed());
            const auto count = qMin<qsizetype>(257, transport.size());
            peerInput.append(transport.first(count)); transport.remove(0, count);
            controller.notifyWritable();
            for (int iteration = 0; iteration < 10000; ++iteration) {
                if (auto action = peer.takeAction()) {
                    FT::OperationResult result;
                    if (action->kind == FT::ActionKind::OfferFile) result.accepted = mode != 2;
                    if (action->kind == FT::ActionKind::WriteAt) {
                        const auto end = qsizetype(action->offset + action->bytes.size());
                        if (received.size() < end) received.resize(end);
                        std::copy(action->bytes.begin(), action->bytes.end(), received.begin() + qsizetype(action->offset));
                    }
                    peer.completeOperation(action->id, std::move(result), now); continue;
                }
                if (peerInput.isEmpty()) break;
                const auto used = peer.consume({peerInput.constData(), peerInput.size()}, now).consumed;
                if (!used) break;
                peerInput.remove(0, qsizetype(used));
            }
            peer.advance(now);
            const auto output = peer.pendingOutput();
            if (!output.empty()) {
                const QByteArray bytes(output.data, output.size);
                peer.acknowledgeOutput(std::size_t(output.size), now);
                controller.acceptBytes(bytes);
            }
        });
        bridge.start(); QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000); bridge.stop();
        QVERIFY2(done.first().first().toBool(), qPrintable(done.first().at(1).toString()));
        QCOMPARE(received, mode == 2 ? QByteArray{} : contents);
        QCOMPARE(controller.progress().completedFiles, quint64(mode == 2 ? 0 : 1));
        QVERIFY(controller.progress().totalBytes);
        QCOMPARE(*controller.progress().totalBytes, quint64(contents.size()));
        QTRY_COMPARE(trace->destroyed.load(), 1);
        QCOMPARE(trace->opened.load(), 1); QVERIFY(trace->workerOnly.load()); QVERIFY(!reserved);
        if (mode == 0) QVERIFY(trace->reads.load() > 1);
        QVERIFY(QFile::rename(path, path + ".moved"));
#endif
    }
    void rejectedSafeOpenSendsNoProtocolBytes()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QByteArray sent; bool reserved = false;
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; };
        channel.reserve = [&](quint64) { reserved = true; return true; };
        channel.release = [&] { reserved = false; };
        channel.coreIdle = [] { return true; };
        channel.pendingWriteBytes = [] { return qint64(0); };
        channel.clearWrites = [] { return true; };
        channel.write = [&](QByteArrayView bytes) -> qint64 { sent.append(bytes); return bytes.size(); };
        SerialFileTransferController controller(channel);
        QSignalSpy done(&controller, &SerialFileTransferController::finished);
        auto trace = std::make_shared<SourceTrace>();
        SerialTransferRequest request; request.source = sourceSpec(directory.path(), "../outside", trace);
        QVERIFY(controller.start(request));
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(!done.first().first().toBool()); QVERIFY(sent.isEmpty()); QVERIFY(!reserved);
        QCOMPARE(trace->opened.load(), 1); QCOMPARE(trace->reads.load(), 0); QVERIFY(trace->workerOnly.load());
    }
    void abortDuringSafeOpenDiscardsLateSource()
    {
#ifndef Q_OS_WIN
        QSKIP("Native safe local Reader is currently Windows-only.");
#else
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("source.bin");
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("late"), qint64(4)); }
        struct Gate { QSemaphore entered, release; };
        auto gate = std::make_shared<Gate>();
        auto trace = std::make_shared<SourceTrace>();
        auto spec = sourceSpec(directory.path(), "source.bin", trace);
        const auto open = spec.open;
        spec.open = [gate, open](QString& error) {
            gate->entered.release();
            if (!gate->release.tryAcquire(1, 5000)) { error = "test gate expired"; return std::unique_ptr<SerialTransferSource>{}; }
            return open(error);
        };
        const auto unblock = qScopeGuard([gate] { gate->release.release(); });
        QByteArray sent; bool reserved = false;
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; };
        channel.reserve = [&](quint64) { reserved = true; return true; };
        channel.release = [&] { reserved = false; };
        channel.coreIdle = [] { return true; };
        channel.pendingWriteBytes = [] { return qint64(0); };
        channel.clearWrites = [] { return true; };
        channel.write = [&](QByteArrayView bytes) -> qint64 { sent.append(bytes); return bytes.size(); };
        SerialFileTransferController controller(channel);
        QSignalSpy done(&controller, &SerialFileTransferController::finished);
        SerialTransferRequest request; request.source = std::move(spec);
        QVERIFY(controller.start(request)); QVERIFY(gate->entered.tryAcquire(1, 1000));
        controller.abort("Original UART connection changed");
        QCOMPARE(done.size(), 1); QVERIFY(!reserved); QVERIFY(sent.isEmpty());
        gate->release.release();
        QTRY_COMPARE(trace->destroyed.load(), 1);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(done.size(), 1); QCOMPARE(trace->reads.load(), 0); QVERIFY(trace->workerOnly.load());
        QVERIFY(sent.isEmpty()); QVERIFY(QFile::rename(path, path + ".moved"));
#endif
    }
};
QTEST_GUILESS_MAIN(SerialFileSourceTests)
#include "SerialFileSourceTests.moc"
