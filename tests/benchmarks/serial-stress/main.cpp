/**
 * @file main.cpp
 * @brief 虚拟串口压力验收：生产接收/解析/渲染通路与独立定速发送线程。
 * @note 仅在显式运行时打开指定端口，不注册默认 CTest。
 */
#include "transport/SerialTransport.h"
#include "session/SessionInputPump.h"
#include "session/InteractiveStreamFramer.h"
#include "core/terminal/TerminalCore.h"
#include "core/ThreadNaming.h"
#include "renderer/TerminalRenderer.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <QWindow>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <psapi.h>
#include <tlhelp32.h>

/** @brief 仅夹具编译启用的端口访问，不向生产 API 暴露原生句柄。 */
class SerialStressPortAccess {
public:
    static QSerialPort& port(SerialTransport& transport) { return transport._port; }
};

namespace {
constexpr qsizetype FrameBytes = 128;

/** @brief CRC32/IEEE：查表实现，避免验证器占用接收热路径。 */
quint32 crc32(QByteArrayView bytes)
{
    static const auto table = [] {
        std::array<quint32, 256> values{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int bit = 0; bit < 8; ++bit)
                c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0u);
            values[i] = c;
        }
        return values;
    }();
    quint32 c = 0xffffffffu;
    for (char b : bytes)
        c = table[(c ^ static_cast<unsigned char>(b)) & 255u] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

QByteArray frame(qint64 sequence)
{
    QByteArray bytes = "NV:" + QByteArray::number(sequence).rightJustified(10, '0') + ':';
    for (int i = 0; i < 103; ++i)
        bytes.append(static_cast<char>('A' + (sequence + i) % 26));
    bytes.append(':');
    bytes += QByteArray::number(crc32(bytes), 16).rightJustified(8, '0');
    bytes += "\r\n";
    return bytes;
}

/** @brief 重组字节流，不把 readyRead 分块误当作串口帧。 */
struct Verifier {
    explicit Verifier(qint64 count) : seen(static_cast<std::size_t>(count), false) {}
    std::vector<bool> seen;
    QByteArray pending;
    QCryptographicHash hash{QCryptographicHash::Sha256};
    qint64 bytes{0}, valid{0}, bad{0}, duplicate{0}, outOfOrder{0}, previous{-1};
    void consume(const QByteArray& data)
    {
        bytes += data.size();
        hash.addData(data);
        pending += data;
        qsizetype offset = 0;
        for (;;) {
            const auto end = pending.indexOf('\n', offset);
            if (end < 0) break;
            const auto line = pending.mid(offset, end - offset + 1);
            offset = end + 1;
            bool ok = false;
            const auto seq = line.mid(3, 10).toLongLong(&ok);
            if (!ok || seq < 0 || seq >= static_cast<qint64>(seen.size())
                || line != frame(seq)) { ++bad; continue; }
            if (seen[static_cast<std::size_t>(seq)]) ++duplicate;
            else { seen[static_cast<std::size_t>(seq)] = true; ++valid; }
            if (seq <= previous) ++outOfOrder;
            previous = seq;
        }
        pending.remove(0, offset);
        if (pending.size() > FrameBytes * 4) { ++bad; pending.clear(); }
    }
    qint64 missing() const { return static_cast<qint64>(seen.size()) - valid; }
};

void events(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

/** @brief 记录线程名称和 ID，便于从 ETW 中剔除发送夹具并区分 Parser。 */
QJsonArray threadDescriptions()
{
    struct CloseKernelHandle { void operator()(void* value) const { CloseHandle(value); } };
    struct FreeDescription { void operator()(wchar_t* value) const { LocalFree(value); } };
    using KernelHandle = std::unique_ptr<void, CloseKernelHandle>;
    QJsonArray result;
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    KernelHandle ownedSnapshot(snapshot);
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (!Thread32First(snapshot, &entry)) return result;
    do {
        if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
        KernelHandle handle(OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID));
        PWSTR description = nullptr;
        QString name;
        if (handle && SUCCEEDED(GetThreadDescription(handle.get(), &description))) {
            std::unique_ptr<wchar_t, FreeDescription> text(description);
            name = QString::fromWCharArray(text.get());
        }
        result.append(QJsonObject{{"id", double(entry.th32ThreadID)}, {"name", name}});
    } while (Thread32Next(snapshot, &entry));
    return result;
}

/** @brief 采样握手：排除语料生成和离线校验，控制文件只位于显式指定路径。 */
bool signalProfilePhase(const QString& base, const QString& phase, const QString& permit)
{
    if (base.isEmpty()) return true;
    QFile marker(base + phase);
    if (!marker.open(QIODevice::WriteOnly) || marker.write("ready\n") != 6 || !marker.flush()) return false;
    marker.close();
    QElapsedTimer timeout;
    timeout.start();
    while (!QFile::exists(base + permit)) {
        if (timeout.elapsed() > 180000) return false;
        events(10);
    }
    return true;
}

double cpuMs()
{
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return -1;
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return static_cast<double>(k.QuadPart + u.QuadPart) / 10000.;
}

double percentile(std::vector<double> values, double p)
{
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>((values.size() - 1) * p)];
}

/** @brief 发送线程拥有自己的串口对象，写入完成后才推进计数。 */
struct Sender {
    std::atomic<bool> done{false}, stop{false};
    std::atomic<qint64> accepted{0}, written{0};
    std::atomic<DWORD> threadId{0};
    QString error;
    double durationMs{0};
    qint64 maxSendChunkBytes{0}, maxSendGapNs{0}, sendsAboveDriverQueue{0}, writeCalls{0};
    std::thread worker;
    ~Sender() { stop = true; if (worker.joinable()) worker.join(); }
    void start(QString portName, int baud, const QByteArray& corpus, bool burst, int chunkBytes)
    {
        worker = std::thread([this, portName, baud, &corpus, burst, chunkBytes] {
            NovaTerm::setCurrentThreadName("nvterm-ser-tx");
            threadId = GetCurrentThreadId();
            QSerialPort port;
            port.setPortName(portName);
            port.setBaudRate(baud);
            port.setDataBits(QSerialPort::Data8);
            port.setParity(QSerialPort::NoParity);
            port.setStopBits(QSerialPort::OneStop);
            port.setFlowControl(QSerialPort::NoFlowControl);
            if (!port.open(QIODevice::ReadWrite) || !port.setBaudRate(baud)) {
                error = port.errorString(); done = true; return;
            }
            QElapsedTimer time;
            time.start();
            qint64 previousSendNs = 0;
            while (!stop && accepted < corpus.size()) {
                const qint64 sent = accepted;
                const qint64 due = burst ? corpus.size()
                    : std::min<qint64>(corpus.size(),
                        (time.nsecsElapsed() * static_cast<qint64>(baud) / 10000000000LL)
                            / FrameBytes * FrameBytes);
                if (due <= sent) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                const qint64 amount = std::min<qint64>(chunkBytes, due - sent);
                const auto n = port.write(corpus.constData() + sent, amount);
                if (n <= 0) { error = port.errorString(); break; }
                const auto now = time.nsecsElapsed();
                maxSendGapNs = std::max(maxSendGapNs, now - previousSendNs);
                previousSendNs = now;
                maxSendChunkBytes = std::max(maxSendChunkBytes, n);
                if (n > 4096) ++sendsAboveDriverQueue;
                ++writeCalls;
                accepted += n;
                while (port.bytesToWrite() > 0 && !stop) {
                    if (!port.waitForBytesWritten(5000)) {
                        error = port.errorString(); stop = true; break;
                    }
                }
                written = accepted.load() - port.bytesToWrite();
            }
            durationMs = static_cast<double>(time.nsecsElapsed()) / 1000000.;
            port.close();
            done = true;
        });
    }
};

/** @brief 对保留历史的最后 512 帧逐字节验证，历史淘汰不视为接收丢帧。 */
QJsonObject validateModel(TerminalCore& core, qint64 totalFrames)
{
    QByteArray text;
    const auto history = core.scrollbackSnapshot();
    const auto first = std::max<NovaTerm::isize>(0, history.lineCount() - 1200);
    for (auto row = first; row < history.lineCount(); ++row) {
        const auto* line = history.lineAt(row);
        if (!line) continue;
        QByteArray segment;
        for (const auto& cell : line->cells) {
            const auto c = cell.chars[0];
            if (c == NovaTerm::WideCharContinuation) continue;
            segment.append(c >= 32 && c < 127 ? static_cast<char>(c) : ' ');
        }
        if (line->hardBreak) {
            while (segment.endsWith(' ')) segment.chop(1);
            text += segment + "\r\n";
        } else text += segment;
    }
    for (int row = 0; row < core.rows(); ++row) {
        QByteArray segment;
        for (int col = 0; col < core.columns(); ++col) {
            NovaTerm::Cell cell;
            if (!core.getCell(row, col, cell)) continue;
            const auto c = cell.chars[0];
            if (c == NovaTerm::WideCharContinuation) continue;
            segment.append(c >= 32 && c < 127 ? static_cast<char>(c) : ' ');
        }
        while (segment.endsWith(' ')) segment.chop(1);
        if (row > 0 && core.rowContinuation(row)) {
            if (text.endsWith("\r\n")) text.chop(2);
        }
        text += segment + "\r\n";
    }
    qint64 missing = 0;
    const auto count = std::min<qint64>(512, totalFrames);
    for (auto seq = totalFrames - count; seq < totalFrames; ++seq)
        if (!text.contains(frame(seq))) ++missing;
    return {{"checkedFrames", double(count)}, {"missingOrCorruptFrames", double(missing)},
            {"historyLines", double(history.lineCount())}, {"columns", core.columns()},
            {"rows", core.rows()}};
}
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    auto args = app.arguments();
    bool deferredVerification = args.removeAll(QStringLiteral("--deferred-verify")) > 0;
    const bool stableTimers = args.removeAll(QStringLiteral("--stable-timers")) > 0;
    QString profileHandshake;
    const int profileIndex = args.indexOf(QStringLiteral("--profile-handshake"));
    if (profileIndex >= 0) {
        if (profileIndex + 1 >= args.size()) return 1;
        profileHandshake = args[profileIndex + 1];
        args.removeAt(profileIndex); args.removeAt(profileIndex);
    }
    int requestedDriverRxQueue = 0;
    const int queueIndex = args.indexOf(QStringLiteral("--driver-rx-queue"));
    if (queueIndex >= 0) {
        bool queueOk = false;
        if (queueIndex + 1 >= args.size()) return 1;
        requestedDriverRxQueue = args[queueIndex + 1].toInt(&queueOk);
        if (!queueOk || requestedDriverRxQueue < 4096 || requestedDriverRxQueue > 4 * 1024 * 1024) return 1;
        args.removeAt(queueIndex); args.removeAt(queueIndex);
    }
    if (args.size() == 4 && args[1] == "--probe") {
        QSerialPort port;
        port.setPortName(args[2]); port.setBaudRate(8000000);
        port.setDataBits(QSerialPort::Data8); port.setParity(QSerialPort::NoParity);
        port.setStopBits(QSerialPort::OneStop); port.setFlowControl(QSerialPort::NoFlowControl);
        port.setReadBufferSize(256 * 1024);
        if (!port.open(QIODevice::ReadWrite)) return 3;
        COMMPROP properties{};
        properties.wPacketLength = sizeof(properties);
        properties.dwProvSpec1 = COMMPROP_INITIALIZED;
        const bool queried = GetCommProperties(reinterpret_cast<HANDLE>(port.handle()), &properties) != FALSE;
        const auto error = queried ? 0 : GetLastError();
        const QJsonObject result{{"port", args[2]}, {"queried", queried}, {"win32Error", double(error)},
            {"driverCurrentRxQueue", double(properties.dwCurrentRxQueue)},
            {"driverCurrentTxQueue", double(properties.dwCurrentTxQueue)},
            {"driverMaxRxQueue", double(properties.dwMaxRxQueue)},
            {"driverMaxTxQueue", double(properties.dwMaxTxQueue)},
            {"qtReadBufferSize", double(port.readBufferSize())}};
        QFile output(args[3]);
        const auto json = QJsonDocument(result).toJson();
        if (!output.open(QIODevice::WriteOnly) || output.write(json) != json.size() || !output.flush()) return 4;
        std::printf("%s driver rxQueue=%lu txQueue=%lu QtReadBuffer=%lld\n", qPrintable(args[2]),
            properties.dwCurrentRxQueue, properties.dwCurrentTxQueue, static_cast<long long>(port.readBufferSize()));
        return queried ? 0 : 5;
    }
    int senderChunkBytes = 16384;
    const int chunkIndex = args.indexOf(QStringLiteral("--tx-chunk"));
    if (chunkIndex >= 0) {
        bool chunkOk = false;
        if (chunkIndex + 1 >= args.size()) return 1;
        senderChunkBytes = args[chunkIndex + 1].toInt(&chunkOk);
        if (!chunkOk || senderChunkBytes < FrameBytes || senderChunkBytes > 65536
            || senderChunkBytes % FrameBytes != 0) return 1;
        args.removeAt(chunkIndex); args.removeAt(chunkIndex);
    }
    if (args.contains("--self-test")) {
        Verifier good(3);
        const auto data = frame(0) + frame(1) + frame(2);
        for (qsizetype i = 0; i < data.size(); i += 17) good.consume(data.mid(i, 17));
        Verifier faults(4);
        auto corrupt = frame(1); corrupt[30] = '!';
        faults.consume(frame(0) + corrupt + frame(2) + frame(2));
        const bool pass = good.valid == 3 && good.bad == 0 && good.missing() == 0
            && faults.missing() == 2 && faults.bad == 1 && faults.duplicate == 1
            && faults.outOfOrder == 1;
        std::printf("verifier self-test: %s\n", pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }
    if (args.size() != 8 && args.size() != 10) {
        std::fprintf(stderr, "usage: serial_stress tx rx baud seconds visible|hidden|raw paced|burst output.json [--bytes total] [--deferred-verify]\n");
        return 1;
    }
    bool baudOk = false, secondsOk = false;
    const int baud = args[3].toInt(&baudOk), seconds = args[4].toInt(&secondsOk);
    const bool visible = args[5] == "visible", burst = args[6] == "burst";
    const bool rawReceiver = args[5] == "raw";
    deferredVerification = deferredVerification || rawReceiver;
    if (!baudOk || !secondsOk || baud <= 0 || baud > 8000000 || seconds < 1 || seconds > 120
        || args[1] == args[2] || (args[5] != "visible" && args[5] != "hidden" && !rawReceiver)
        || (args[6] != "paced" && args[6] != "burst")) return 1;
    qint64 totalBytes = static_cast<qint64>(baud) * seconds / 10 / FrameBytes * FrameBytes;
    if (args.size() == 10) {
        bool sizeOk = false;
        totalBytes = args[9].toLongLong(&sizeOk);
        if (args[8] != "--bytes" || !sizeOk || totalBytes < FrameBytes
            || totalBytes > 256LL * 1024 * 1024 || totalBytes % FrameBytes != 0) return 1;
    }
    const qint64 count = totalBytes / FrameBytes;
    const qint64 timeoutMs = burst ? 120000 : totalBytes * 10000 / baud + 30000;
    QByteArray corpus;
    corpus.reserve(count * FrameBytes);
    for (qint64 seq = 0; seq < count; ++seq) corpus += frame(seq);
    const auto expectedHash = QCryptographicHash::hash(corpus, QCryptographicHash::Sha256);
    bool timerPolicyApplied = false;
#ifdef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
    if (stableTimers) {
        PROCESS_POWER_THROTTLING_STATE state{};
        state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        state.StateMask = 0;
        timerPolicyApplied = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
            &state, static_cast<DWORD>(sizeof(state))) != FALSE;
        if (!timerPolicyApplied) return 6;
    }
#endif
    timeBeginPeriod(1);
    TerminalCore core(160, 40);
    core.setScrollbackLimit(10000);
    TerminalRenderer renderer(&core);
    renderer.setTargetRefreshRate(60);
    renderer.setWindowTitle(QStringLiteral("NovaTerm COM stress %1 -> %2 %3 %4")
        .arg(args[1], args[2]).arg(baud).arg(args[5]));
    renderer.resize(1500, 800);
    bool renderFailed = false;
    QObject::connect(&renderer, &QRhiWidget::renderFailed, [&] { renderFailed = true; });
    if (!rawReceiver) {
        renderer.show();
        events(750);
        if (renderFailed || !renderer.windowHandle() || !renderer.windowHandle()->isExposed()) return 2;
        if (!visible) renderer.hide();
    }
    events(100);
    SerialConfig config;
    config.portName = args[2]; config.baudRate = baud;
    SerialTransport transport(config);
    InteractiveStreamFramer framer;
    SessionInputPump pump(&transport, &core, &framer);
    Verifier verifier(count);
    QByteArray captured;
    if (deferredVerification) captured.reserve(corpus.size());
    QStringList errors;
    QObject::connect(&transport, &ITransport::errorOccurred, [&](const QString& error) { errors += error; });
    QObject::connect(&pump, &SessionInputPump::overload, [&](const QString& error) { errors += error; });
    QObject::connect(&transport, &ITransport::readyRead, [&](const QByteArray& data) {
        if (deferredVerification) captured += data;
        else verifier.consume(data);
    });
    if (!rawReceiver) pump.start();
    if (!transport.connectToHost()) return 3;
    for (int i = 0; i < 100 && !transport.isConnected() && errors.isEmpty(); ++i) events(10);
    if (!transport.isConnected()) {
        std::fprintf(stderr, "open failed: %s\n", qPrintable(errors.join(';'))); return 3;
    }
    auto& receiverPort = SerialStressPortAccess::port(transport);
    const auto receiverHandle = reinterpret_cast<HANDLE>(receiverPort.handle());
    COMMPROP queueBefore{};
    queueBefore.wPacketLength = sizeof(queueBefore);
    queueBefore.dwProvSpec1 = COMMPROP_INITIALIZED;
    const bool queriedBefore = GetCommProperties(receiverHandle, &queueBefore) != FALSE;
    bool queueResizeAccepted = true;
    if (requestedDriverRxQueue > 0)
        queueResizeAccepted = SetupComm(receiverHandle, static_cast<DWORD>(requestedDriverRxQueue), 4096) != FALSE;
    const DWORD queueResizeError = queueResizeAccepted ? 0 : GetLastError();
    COMMPROP queueAfter{};
    queueAfter.wPacketLength = sizeof(queueAfter);
    queueAfter.dwProvSpec1 = COMMPROP_INITIALIZED;
    const bool queriedAfter = GetCommProperties(receiverHandle, &queueAfter) != FALSE;
    const bool driverQueueConfirmed = requestedDriverRxQueue == 0
        || (queueResizeAccepted && queriedAfter && queueAfter.dwCurrentRxQueue >= static_cast<DWORD>(requestedDriverRxQueue));
    std::printf("RX queue: before=%lu requested=%d actual=%lu confirmed=%d\n",
        queueBefore.dwCurrentRxQueue, requestedDriverRxQueue, queueAfter.dwCurrentRxQueue,
        driverQueueConfirmed ? 1 : 0);
    if (!driverQueueConfirmed) {
        const QJsonObject rejection{{"pass", false}, {"driverQueueExclusionConfirmed", false},
            {"requestedDriverRxQueue", requestedDriverRxQueue},
            {"beforeRxQueue", double(queueBefore.dwCurrentRxQueue)},
            {"actualRxQueue", double(queueAfter.dwCurrentRxQueue)},
            {"setupCommAccepted", queueResizeAccepted}, {"setupCommError", double(queueResizeError)},
            {"propertiesQueriedBefore", queriedBefore}, {"propertiesQueriedAfter", queriedAfter},
            {"note", QStringLiteral("Driver queue enlargement was not verified; no test bytes sent.")}};
        QFile output(args[7]);
        const auto json = QJsonDocument(rejection).toJson();
        if (!output.open(QIODevice::WriteOnly) || output.write(json) != json.size() || !output.flush()) return 4;
        return 7;
    }
    const auto namedThreads = threadDescriptions();
    if (!signalProfilePhase(profileHandshake, ".ready", ".go")) return 8;
    const auto initialRender = renderer.renderStatistics();
    const auto initialQueue = core.queueStatistics();
    const double initialCpu = cpuMs();
    QElapsedTimer wall; wall.start();
    Sender sender;
    sender.start(args[1], baud, corpus, burst, senderChunkBytes);
    std::vector<double> tickIntervals;
    qint64 priorNs = wall.nsecsElapsed(), lastReceivedNs = priorNs;
    qint64 previousBytes = 0, peakQueue = 0;
    quint64 peakRss = 0;
    bool timeout = false;
    QEventLoop loop;
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer); timer.setInterval(10);
    QObject::connect(&timer, &QTimer::timeout, [&] {
        const auto now = wall.nsecsElapsed();
        tickIntervals.push_back(double(now - priorNs) / 1000000.); priorNs = now;
        peakQueue = std::max<qint64>(peakQueue, core.queueStatistics().queuedBytes);
        PROCESS_MEMORY_COUNTERS memory{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
            peakRss = std::max<quint64>(peakRss, memory.WorkingSetSize);
        const qint64 observedBytes = deferredVerification ? captured.size() : verifier.bytes;
        if (observedBytes != previousBytes) { lastReceivedNs = now; previousBytes = observedBytes; }
        if (wall.elapsed() > timeoutMs) { timeout = true; sender.stop = true; loop.quit(); }
        if (sender.done && (observedBytes >= corpus.size() || now - lastReceivedNs > 2000000000LL)
            && pump.statistics().pendingBytes == 0 && core.queueStatistics().queuedBytes == 0)
            loop.quit();
    });
    timer.start(); loop.exec(); timer.stop();
    sender.stop = true;
    if (sender.worker.joinable()) sender.worker.join();
    const bool idle = core.waitForIdle(10000);
    const double elapsedMs = double(wall.nsecsElapsed()) / 1000000.;
    const double usedCpu = cpuMs() - initialCpu;
    events(150);
    const auto render = renderer.renderStatistics();
    const auto queue = core.queueStatistics();
    const auto stats = pump.statistics();
    if (!signalProfilePhase(profileHandshake, ".done", ".release")) return 8;
    if (deferredVerification) verifier.consume(captured);
    const auto model = rawReceiver
        ? QJsonObject{{"checkedFrames", 0}, {"missingOrCorruptFrames", 0}}
        : validateModel(core, count);
    const bool hashMatches = verifier.hash.result() == expectedHash;
    const bool pass = !timeout && idle && errors.isEmpty() && sender.error.isEmpty()
        && sender.written == corpus.size() && verifier.bytes == corpus.size() && hashMatches
        && verifier.missing() == 0 && verifier.bad == 0 && verifier.duplicate == 0
        && verifier.outOfOrder == 0 && verifier.pending.isEmpty()
        && (rawReceiver || stats.acceptedBytes == static_cast<quint64>(corpus.size())) && stats.overloadCount == 0
        && (rawReceiver || queue.totalDequeued - initialQueue.totalDequeued == static_cast<quint64>(corpus.size()))
        && model["missingOrCorruptFrames"].toInt() == 0 && !renderFailed
        && (!visible || render.framesRendered > initialRender.framesRendered);
    QJsonObject result{{"pass", pass}, {"tx", args[1]}, {"rx", args[2]}, {"baud", baud},
        {"visible", visible}, {"burst", burst}, {"durationTargetSeconds", double(totalBytes) * 10. / baud},
        {"rawReceiver", rawReceiver}, {"deferredVerification", deferredVerification},
        {"pid", double(GetCurrentProcessId())}, {"mainThreadId", double(GetCurrentThreadId())},
        {"senderThreadId", double(sender.threadId.load())}, {"threads", namedThreads},
        {"senderChunkBytes", senderChunkBytes},
        {"requestedDriverRxQueue", requestedDriverRxQueue},
        {"driverRxQueueBefore", double(queueBefore.dwCurrentRxQueue)},
        {"driverRxQueueActual", double(queueAfter.dwCurrentRxQueue)},
        {"driverQueueExclusionConfirmed", requestedDriverRxQueue > 0 && driverQueueConfirmed},
        {"driverQueueResizeAccepted", queueResizeAccepted},
        {"stableTimersRequested", stableTimers}, {"timerPolicyApplied", timerPolicyApplied},
        {"maxSendChunkBytes", double(sender.maxSendChunkBytes)},
        {"maxSendGapMs", double(sender.maxSendGapNs) / 1000000.},
        {"sendsAboveDriverQueue", double(sender.sendsAboveDriverQueue)},
        {"senderWriteCalls", double(sender.writeCalls)},
        {"expectedBytes", double(corpus.size())}, {"expectedFrames", double(count)},
        {"sentAcceptedBytes", double(sender.accepted.load())}, {"sentWrittenBytes", double(sender.written.load())},
        {"receivedBytes", double(verifier.bytes)}, {"validFrames", double(verifier.valid)},
        {"missingFrames", double(verifier.missing())}, {"corruptFrames", double(verifier.bad)},
        {"duplicateFrames", double(verifier.duplicate)}, {"outOfOrderFrames", double(verifier.outOfOrder)},
        {"partialFrameBytes", double(verifier.pending.size())}, {"hashMatches", hashMatches},
        {"sha256", QString::fromLatin1(expectedHash.toHex())}, {"pumpAcceptedBytes", double(stats.acceptedBytes)},
        {"pumpPendingBytes", double(stats.pendingBytes)}, {"pumpPauseCount", double(stats.pauseCount)},
        {"pumpOverloadCount", double(stats.overloadCount)}, {"coreDequeuedBytes", double(queue.totalDequeued - initialQueue.totalDequeued)},
        {"coreQueuePeakBytes", double(peakQueue)}, {"coreHighWaterBytes", double(queue.highWatermark)},
        {"coreProducerWaits", double(queue.producerWaits - initialQueue.producerWaits)},
        {"coreIdle", idle}, {"timeout", timeout}, {"senderError", sender.error},
        {"receiveErrors", errors.join(';')}, {"sendWallMs", sender.durationMs}, {"drainWallMs", elapsedMs},
        {"throughputBytesPerSecond", double(verifier.bytes) * 1000. / elapsedMs},
        {"cpuMs", usedCpu}, {"cpuOneCorePercent", usedCpu * 100. / elapsedMs},
        {"peakWorkingSetBytes", double(peakRss)}, {"eventLoopTickP95Ms", percentile(tickIntervals, .95)},
        {"eventLoopTickMaxMs", percentile(tickIntervals, 1.)},
        {"renderedFrames", double(render.framesRendered - initialRender.framesRendered)},
        {"cpuFrameP95Ms", double(render.cpuFrameP95Nanoseconds) / 1000000.},
        {"gpuUploadBytes", double(render.gpuUploadBytes - initialRender.gpuUploadBytes)},
        {"parsedModelTail", model}};
    QFile output(args[7]);
    const auto json = QJsonDocument(result).toJson();
    if (!output.open(QIODevice::WriteOnly) || output.write(json) != json.size() || !output.flush()) return 4;
    std::printf("%s %s->%s baud=%d %s %s frames=%lld/%lld loss=%lld bad=%lld bytes=%lld rate=%.0f B/s modelMissing=%d\n",
        pass ? "PASS" : "FAIL", qPrintable(args[1]), qPrintable(args[2]), baud,
        rawReceiver ? "raw" : (visible ? "visible" : "hidden"), burst ? "burst" : "paced",
        static_cast<long long>(verifier.valid), static_cast<long long>(count),
        static_cast<long long>(verifier.missing()), static_cast<long long>(verifier.bad),
        static_cast<long long>(verifier.bytes), double(verifier.bytes) * 1000. / elapsedMs,
        model["missingOrCorruptFrames"].toInt());
    pump.stop(); transport.disconnect(); timeEndPeriod(1);
    return pass ? 0 : 5;
}
