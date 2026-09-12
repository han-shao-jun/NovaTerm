// SshTransport 失败路径控制台回归检查。
//
// 不依赖真实 SSH 服务器，验证最容易出错的部分：
//   • 无效配置 → connectToHost() 同步失败并给出 errorString；
//   • 连接被拒（127.0.0.1:1）→ 异步 errorOccurred 且线程正常回收、不崩溃。
//
// 运行：build/bin/novaterm_ssh_transport_check.exe
#include "transport/SshWorkerWakeup.h"
#include "transport/SshTransport.h"
#include <libssh/libssh.h>
#include <thread>
#include <chrono>
#include "transport/SshCommandCompletion.h"
#include "transport/SshMonitorProtocol.h"
#include "service/LinuxResourceData.h"
#include "service/ResourcePrefetchSchedule.h"

#include <QCoreApplication>
#include <QTimer>
#include <QEvent>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QFile>
#include <QProcess>
#include <ctime>

#include <cstdio>

class SshTransportTestAccess
{
public:
    static void knownHosts(SshTransport& transport, const QString& path)
    { transport._knownHostsPath = path; }
    static void append(SshTransport& transport, const QByteArray& bytes)
    { transport.emitReadyRead(bytes); }
    static qsizetype capacity(const SshTransport& transport)
    { return transport.inboundCapacity(); }
    static void close(SshTransport& transport)
    {
        QMutexLocker lock(&transport._inboundMutex);
        transport._connected.store(true);
        transport._inboundClosed = true;
        transport.scheduleInboundLocked();
    }
};

// 显式参数才运行隔离本机 SSH；不使用用户密钥，也不写用户 known_hosts。
int localSshCheck(QCoreApplication& app)
{
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    for (const QString& name : {QStringLiteral("host"), QStringLiteral("client")}) {
        if (QProcess::execute(QStringLiteral("ssh-keygen"),
                {QStringLiteral("-q"), QStringLiteral("-t"), QStringLiteral("ed25519"),
                 QStringLiteral("-N"), QString{}, QStringLiteral("-f"),
                 directory.filePath(name)}) != 0)
            return 2;
    }
    QFile config(directory.filePath(QStringLiteral("sshd_config")));
    if (!config.open(QIODevice::WriteOnly)) return 2;
    config.write((QStringLiteral("ListenAddress 127.0.0.1\nPort 42222\nHostKey ")
        + directory.filePath(QStringLiteral("host"))
        + QStringLiteral("\nAuthorizedKeysFile ") + directory.filePath(QStringLiteral("client.pub"))
        + QStringLiteral("\nPidFile ") + directory.filePath(QStringLiteral("pid"))
        + QStringLiteral("\nStrictModes no\nPasswordAuthentication no\nKbdInteractiveAuthentication no\nUsePAM no\nLogLevel ERROR\n")).toUtf8());
    config.close();
    QProcess server;
    server.start(QStringLiteral("/usr/sbin/sshd"),
                 {QStringLiteral("-D"), QStringLiteral("-e"), QStringLiteral("-f"), config.fileName()});
    if (!server.waitForStarted() || server.waitForFinished(200)) {
        std::fprintf(stderr, "[local-ssh] unavailable: %s\n", server.readAllStandardError().constData());
        return 2;
    }
    SshConfig settings;
    settings.host = QStringLiteral("127.0.0.1");
    settings.port = 42222;
    settings.username = QString::fromLocal8Bit(qgetenv("USER"));
    settings.authMethod = QStringLiteral("publickey");
    settings.privateKeyPath = directory.filePath(QStringLiteral("client"));
    SshTransport transport(settings);
    SshTransportTestAccess::knownHosts(transport, directory.filePath(QStringLiteral("known_hosts")));
    QObject::connect(&transport, &SshTransport::hostKeyRequired, &transport,
                     [&] { transport.acceptHostKey(); });
    bool error = false;
    QObject::connect(&transport, &SshTransport::errorOccurred, [&](const QString& message) {
        std::fprintf(stderr, "[local-ssh] %s\n", message.toUtf8().constData());
        error = true;
    });
    const auto waitUntil = [&](const auto& predicate, int timeout) {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && !error && timer.elapsed() < timeout) {
            app.processEvents();
            QThread::msleep(1);
        }
        return predicate() && !error;
    };
    int result = 0;
    if (!transport.connectToHost() || !waitUntil([&] { return transport.isConnected(); }, 15000)) {
        result = 1;
    } else {
        const auto cpuStart = std::clock();
        QElapsedTimer idle;
        idle.start();
        // 阻塞 Qt 事件循环而非主动轮询，用进程 CPU 时间测 idle。
        QEventLoop idleLoop;
        QTimer::singleShot(2000, &idleLoop, &QEventLoop::quit);
        idleLoop.exec();
        std::printf("[local-ssh] idle CPU=%.3f%% over %lld ms\n",
            100.0 * double(std::clock() - cpuStart) / CLOCKS_PER_SEC
                / (double(idle.elapsed()) / 1000.0), static_cast<long long>(idle.elapsed()));
        QByteArray tail;
        QByteArray framedInput;
        qint64 expectedPayload = 0;
        qint64 checkedPayload = 0;
        bool payloadStarted = false;
        bool payloadInvalid = false;
        qint64 received = 0;
        QObject::connect(&transport, &SshTransport::readyRead, [&](const QByteArray& bytes) {
            received += bytes.size();
            tail.append(bytes);
            if (tail.size() > 1024) tail = tail.right(1024);
            if (expectedPayload > 0 && checkedPayload < expectedPayload) {
                framedInput.append(bytes);
                if (!payloadStarted) {
                    const QByteArray marker("\r\nBEGIN_LOCAL\r\n");
                    const auto offset = framedInput.indexOf(marker);
                    if (offset < 0) {
                        framedInput = framedInput.right(marker.size());
                        return;
                    }
                    framedInput.remove(0, offset + marker.size());
                    payloadStarted = true;
                }
                const auto count = std::min<qint64>(framedInput.size(), expectedPayload - checkedPayload);
                for (qint64 index = 0; index < count; ++index)
                    payloadInvalid |= framedInput[qsizetype(index)] != 'x';
                checkedPayload += count;
                framedInput.clear();
            }
        });
        transport.write("stty -echo; printf '\\nREADY_LOCAL\\n'\n");
        if (!waitUntil([&] { return tail.contains("\r\nREADY_LOCAL\r\n"); }, 3000)) result = 1;
        for (int mib : {10, 50, 100}) {
            tail.clear();
            received = 0;
            expectedPayload = qint64(mib) * 1024 * 1024;
            checkedPayload = 0;
            payloadStarted = false;
            payloadInvalid = false;
            framedInput.clear();
            QElapsedTimer timer;
            timer.start();
            transport.write("printf '\\nBEGIN_LOCAL\\n'; head -c " + QByteArray::number(mib * 1024 * 1024)
                + " /dev/zero | tr '\\000' x; printf '\\nDONE_LOCAL\\n'\n");
            if (!waitUntil([&] { return tail.contains("DONE_LOCAL"); }, 60000)
                || checkedPayload != expectedPayload || payloadInvalid) result = 1;
            std::printf("[local-ssh] %d MiB: %lld bytes / %lld ms\n", mib,
                static_cast<long long>(received), static_cast<long long>(timer.elapsed()));
        }
        expectedPayload = 0;
        tail.clear();
        transport.write("yes x\n");
        if (!waitUntil([&] { return received > 0 && tail.contains("x"); }, 3000)) result = 1;
        bool commandDone = false;
        bool monitorDone = false;
        QObject::connect(&transport, &SshTransport::commandFinished,
            [&](quint64 id, const QByteArray& output, const QByteArray&, const QString& errorText) {
                if (id == 101) {
                    commandDone = errorText.isEmpty() && output == "auxiliary-ok";
                    if (!commandDone) result = 1;
                }
            });
        QObject::connect(&transport, &SshTransport::resourceSampleFinished,
            [&](quint64 id, const QByteArray& output, const QString& errorText) {
                if (id == 102) {
                    NovaTerm::LinuxResource::Sample sample;
                    monitorDone = errorText.isEmpty()
                        && NovaTerm::LinuxResource::parseMetrics(output, sample);
                    if (!monitorDone) result = 1;
                }
            });
        transport.resizeTerminal(91, 31);
        transport.startResourceMonitoring();
        if (!transport.executeCommand(101, QByteArrayLiteral("printf auxiliary-ok"))
            || !transport.requestResourceSample(102)
            || !waitUntil([&] { return commandDone && monitorDone; }, 7000)) result = 1;
        transport.stopResourceMonitoring();
        std::printf("[local-ssh] flood auxiliary-command=%d monitor=%d\n",
                    int(commandDone), int(monitorDone));
        QElapsedTimer interrupt;
        interrupt.start();
        transport.write(QByteArray(1, '\x03'));
        transport.write("printf '\\nINTERRUPTED_LOCAL\\n'\n");
        if (!waitUntil([&] { return tail.contains("INTERRUPTED_LOCAL"); }, 3000)) result = 1;
        std::printf("[local-ssh] Ctrl+C completion=%lld ms\n", static_cast<long long>(interrupt.elapsed()));
        tail.clear();
        transport.write("stty size; printf '\\nSIZE_LOCAL\\n'\n");
        if (!waitUntil([&] { return tail.contains("SIZE_LOCAL"); }, 3000)
            || !tail.contains("31 91")) result = 1;
        std::printf("[local-ssh] resize verified=%d\n", int(tail.contains("31 91")));
        // 独立的 32 目标 Ninja 构建，检查真实 CR 进度输出及 Shell 完成标记。
        QFile source(directory.filePath(QStringLiteral("fixture.cpp")));
        QFile ninja(directory.filePath(QStringLiteral("build.ninja")));
        if (!source.open(QIODevice::WriteOnly) || !ninja.open(QIODevice::WriteOnly)) {
            result = 1;
        } else {
            source.write("int fixture(int value) { return value * value + 1; }\n");
            source.close();
            ninja.write("rule compile\n  command = c++ -c $in -o $out\n");
            for (int index = 0; index < 32; ++index)
                ninja.write("build output" + QByteArray::number(index) + ".o: compile fixture.cpp\n");
            ninja.close();
            tail.clear();
            transport.write("ninja -C '" + directory.path().toUtf8()
                + "' -j 2 && printf '\\nNINJA_LOCAL_OK\\n'\n");
            const bool built = waitUntil([&] { return tail.contains("NINJA_LOCAL_OK"); }, 15000);
            if (!built) result = 1;
            std::printf("[local-ssh] isolated Ninja build=%d\n", int(built));
        }
    }
    transport.disconnect();
    server.terminate();
    if (!server.waitForFinished(3000)) { server.kill(); server.waitForFinished(); }
    return result;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--local-ssh-check")))
        return localSshCheck(app);
    int failures = 0;

    // 初始只留给面板；详情分批预热，迟到时也不能集中补发。
    // 断言按常量推导，调参只需改 PrefetchSchedule 本身。
    {
        using NovaTerm::LinuxResource::PrefetchSchedule;
        // 两条硬约束：连接后 2~3 秒内点开详情要已有内容（首批不晚、总时长有界），
        // 且批次之间保持足够间隔，不在远端短时抬高 CPU。
        const int totalReadyMs = PrefetchSchedule::InitialDelayMs
            + (PrefetchSchedule::BatchCount - 1) * PrefetchSchedule::BatchSpacingMs;
        if (PrefetchSchedule::InitialDelayMs > 500 || totalReadyMs > 3000
            || PrefetchSchedule::BatchSpacingMs < 500)
            ++failures;

        PrefetchSchedule schedule;
        if (schedule.ready(0) || schedule.ready(PrefetchSchedule::InitialDelayMs - 1))
            ++failures;
        for (int batch = 0; batch < PrefetchSchedule::BatchCount; ++batch) {
            const int due = PrefetchSchedule::InitialDelayMs
                + batch * PrefetchSchedule::BatchSpacingMs;
            if (schedule.batch() != batch || !schedule.ready(due))
                ++failures;
            schedule.started(due);
            if (schedule.ready(due + 10))
                ++failures;
            schedule.finished(due + 40);
            if (!schedule.complete()
                && schedule.ready(due + PrefetchSchedule::BatchSpacingMs - 1))
                ++failures;
        }
        if (!schedule.complete() || schedule.ready(totalReadyMs + 1000))
            ++failures;

        // 慢批次：完成后按冷却时间重排，不立刻补发，也不吞掉批间隔。
        PrefetchSchedule delayed;
        delayed.started(PrefetchSchedule::InitialDelayMs);
        delayed.finished(3000);
        const int slowDue = 3000 + PrefetchSchedule::CompletionGapMs;
        if (delayed.ready(slowDue - 1) || !delayed.ready(slowDue))
            ++failures;
        delayed.started(slowDue);
        delayed.finished(slowDue + 10);
        const int nextDue = slowDue + PrefetchSchedule::BatchSpacingMs;
        if (delayed.ready(nextDue - 1) || !delayed.ready(nextDue))
            ++failures;
        std::printf("[resource-prefetch] startup spacing/slow completion/no catch-up checked\n");
    }

    // 分批次序本身就是体验约束：概览（os/kernel/host/arch/connection）必须最先，
    // 最重的 lspci 排最后；每批都要以 @@done 收尾，预取才认这一批有效。
    {
        using namespace NovaTerm::LinuxResource;
        using NovaTerm::LinuxResource::PrefetchSchedule;
        const QByteArray overview = staticCommand(0);
        const QByteArray last = staticCommand(PrefetchSchedule::BatchCount - 1);
        if (!overview.contains("@@os") || !overview.contains("@@connection")
            || overview.contains("lspci") || overview.contains("@@gpu")
            || !last.contains("lspci") || !last.contains("@@gpu")
            || !staticCommand(PrefetchSchedule::BatchCount).isEmpty())
            ++failures;
        for (int batch = 0; batch < PrefetchSchedule::BatchCount; ++batch) {
            if (!staticCommand(batch).contains("@@done"))
                ++failures;
        }
        std::printf("[resource-prefetch] batch order overview-first/lspci-last/done-marker checked\n");
    }

    // 显式 disconnect 不发布 disconnected，也必须使辅助数据缓存的代际失效。
    {
        SshTransport transport(SshConfig{});
        const auto generation = transport.connectionGeneration();
        transport.disconnect();
        if (transport.connectionGeneration() == generation)
            ++failures;
    }

    // guest 已包含在 user/nice 中；按区间而非开机累计值计算分类占用。
    {
        using namespace NovaTerm::LinuxResource;
        CpuTicks ticks{};
        if (!cpuTicks("cpu 120 12 36 450 8 4 6 4 90 8\ncpu0 1 2 3 4\n", ticks)
            || ticks != CpuTicks{120, 12, 36, 450, 8, 4, 6, 4})
            ++failures;
        const CpuTicks previous{100, 10, 30, 400, 5, 2, 3, 0};
        if (cpuUsage(ticks, previous)
            != "CPUUSE\t22.2\t6.7\t2.2\t55.6\t3.3\t10.0\n")
            ++failures;
        if (!cpuUsage(previous, ticks).isEmpty()
            || !cpuUsage(ticks, ticks).isEmpty()
            || cpuTicks("cpu 1 bad 2 3", ticks))
            ++failures;
        std::printf("[linux-resource] CPU delta/guest/reset checked\n");

        // MemAvailable=0 是有效值；仅缺字段才回退，网卡冒号与额外空白可解析。
        const QByteArray raw = "@@stat\ncpu 100 10 30 400 5 2 3 0 80 5\n"
            "@@meminfo\nMemTotal: 1000 kB\nMemFree: 100 kB\n"
            "Buffers: 20 kB\nCached: 200 kB\nSReclaimable: 30 kB\n"
            "SwapTotal: 80 kB\nSwapFree: 20 kB\n"
            " eth0: 1234 0 0 0 0 0 0 0 5678 0 0 0 0 0 0 0\n"
            " lo: 99 0 0 0 0 0 0 0 99 0 0 0 0 0 0 0\n"
            "@@loadavg\n0.12 0.34 0.56 1/10 99\n@@uptime\n123.45 80.00\n";
        Sample sample;
        if (!parseMetrics(raw, sample) || sample.cpuTotal != 550
            || sample.cpuIdle != 405 || sample.memoryAvailableKiB != 350
            || sample.cacheKiB != 250 || sample.swapFreeKiB != 20
            || sample.networks.size() != 1 || sample.networks[0].receivedBytes != 1234
            || sample.networks[0].sentBytes != 5678 || sample.load != "0.12 0.34 0.56")
            ++failures;
        QByteArray zeroAvailable = raw;
        zeroAvailable.replace("MemTotal:", "MemAvailable: 0 kB\nMemTotal:");
        if (!parseMetrics(zeroAvailable, sample) || sample.memoryAvailableKiB != 0
            || parseMetrics("@@stat\ncpu 1 2 3 4\n", sample))
            ++failures;
        parseMetrics(raw, sample);
        const QByteArray fixed = "@@os\nPRETTY_NAME=\"Test Linux\"\n@@cpuinfo\n"
            "processor : 0\nmodel name : Test CPU\nprocessor : 1\n"
            "@@kernel\n6.8\n@@connection\n1.2.3.4 123 10.0.0.1 22\n";
        const QByteArray slow = "@@frequency\n1200000\n@@filesystems\n"
            "Filesystem 1024-blocks Used Available Capacity Mounted on\n"
            "/dev/root 1000 300 700 30% /a b\n";
        const auto detail = information(fixed, slow, sample, {});
        if (!detail.contains("CPU\tTest CPU\t2\t1200.0\t")
            || !detail.contains("MEM\t1000\t650\t350\t65.0\t250\n")
            || !detail.contains("FS\t/dev/root\t1000\t30%\t700\t/a b\n")
            || !detail.contains("Test Linux\t6.8\t\t10.0.0.1\t"))
            ++failures;
        std::printf("[linux-resource] memory/network/static/frequency/filesystem checked\n");
    }

    // GUI 交付暂停/恢复、EOF 排序、世代失效及大流量完整性。
    for (int mib : {10, 50, 100}) {
        SshTransport transport(SshConfig{});
        qint64 received = 0;
        int deliveries = 0;
        bool ended = false;
        QObject::connect(&transport, &SshTransport::readyRead,
            [&](const QByteArray& bytes) {
                for (char byte : bytes) {
                    if (byte != static_cast<char>((received / 4096) % 127))
                        ++failures;
                    ++received;
                }
                ++deliveries;
            });
        QObject::connect(&transport, &SshTransport::disconnected, [&] {
            ended = true;
            if (received != qint64(mib) * 1024 * 1024)
                ++failures;
        });
        transport.setReadPaused(true);
        SshTransportTestAccess::append(transport, QByteArray(4096, 0));
        QCoreApplication::sendPostedEvents(&transport, QEvent::MetaCall);
        if (received != 0)
            ++failures;
        transport.setReadPaused(false);
        QElapsedTimer timer;
        timer.start();
        for (qint64 sent = 4096; sent < qint64(mib) * 1024 * 1024; sent += 4096) {
            while (SshTransportTestAccess::capacity(transport) < 4096)
                QCoreApplication::sendPostedEvents(&transport, QEvent::MetaCall);
            SshTransportTestAccess::append(transport,
                QByteArray(4096, static_cast<char>((sent / 4096) % 127)));
        }
        SshTransportTestAccess::close(transport);
        while (!ended)
            QCoreApplication::sendPostedEvents(&transport, QEvent::MetaCall);
        if (deliveries >= mib * 1024 / 4)
            ++failures;
        std::printf("[inbound] %d MiB complete, deliveries=%d, elapsed=%lld ms\n",
                    mib, deliveries, static_cast<long long>(timer.elapsed()));
        SshTransportTestAccess::append(transport, QByteArrayLiteral("stale"));
        transport.disconnect();
        QCoreApplication::sendPostedEvents(&transport, QEvent::MetaCall);
        if (received != qint64(mib) * 1024 * 1024)
            ++failures;
    }

    // 控制通知可先于等待到达，多次通知只保留一个可读标记。
    {
        SshWorkerWakeup wakeup;
        ssh_event event = ssh_event_new();
        int callbacks = 0;
        struct Context { SshWorkerWakeup* wakeup; int* callbacks; } context{&wakeup, &callbacks};
        if (!wakeup.valid() || !event
            || ssh_event_add_fd(event, wakeup.descriptor(), POLLIN,
                [](socket_t, int, void* data) {
                    auto& value = *static_cast<Context*>(data);
                    value.wakeup->consume();
                    ++*value.callbacks;
                    return 0;
                }, &context) != SSH_OK) {
            ++failures;
        } else {
            for (int i = 0; i < 10000; ++i)
                wakeup.notify();
            ssh_event_dopoll(event, 1000);
            if (callbacks != 1)
                ++failures;
            ssh_event_dopoll(event, 30);
            if (callbacks != 1)
                ++failures;
            std::thread producer([&wakeup] {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                wakeup.notify();
            });
            ssh_event_dopoll(event, 1000);
            producer.join();
            if (callbacks != 2)
                ++failures;
            ssh_event_remove_fd(event, wakeup.descriptor());
        }
        if (event)
            ssh_event_free(event);
        std::printf("[worker-wakeup] early/coalesced/blocked notification checked\n");
    }

    // ── 单次命令完成顺序：EOF 不等于 exit-status ───────────
    {
        SshCommandCompletion completion;
        completion.observeOutputEnd();
        if (completion.result() != SshCommandCompletion::Result::Pending)
            ++failures;
        completion.observeExitStatus(0);
        if (completion.result() != SshCommandCompletion::Result::Exited
            || completion.exitStatus().value_or(-1) != 0) {
            ++failures;
        }

        completion.reset();
        completion.observeExitStatus(7);
        if (completion.result() != SshCommandCompletion::Result::Pending)
            ++failures;
        completion.observeOutputEnd();
        if (completion.result() != SshCommandCompletion::Result::Exited
            || completion.exitStatus().value_or(-1) != 7) {
            ++failures;
        }

        completion.reset();
        completion.observeOutputEnd();
        completion.observeRemoteClose();
        if (completion.result()
            != SshCommandCompletion::Result::MissingExitStatus) {
            ++failures;
        }
        std::printf("[command-completion] EOF/status ordering checked\n");
    }

    // ── 协议解析：分片、合帧、错误帧和上限 ──────────────────
    {
        SshMonitorFrameParser parser;
        auto result = parser.append(
            QByteArrayLiteral("__NOVATERM_METRICS_BEG"));
        if (!result.frames.isEmpty()
            || result.error != SshMonitorFrameParser::Error::None)
            ++failures;
        result = parser.append(QByteArrayLiteral(
            "IN__\t7\nCPU\t100\t20\nMEM\t1000\t600\t0\t0\n"
            "__NOVATERM_METRICS_END__\t7\n"
            "__NOVATERM_METRICS_BEGIN__\t8\nNET\teth0\t10\t20\n"
            "__NOVATERM_METRICS_END__\t8\n"));
        if (result.error != SshMonitorFrameParser::Error::None
            || result.frames.size() != 2
            || result.frames[0].requestId != 7
            || !result.frames[0].payload.contains("MEM\t1000")
            || result.frames[1].requestId != 8) {
            ++failures;
        }

        result = parser.append(QByteArrayLiteral(
            "__NOVATERM_METRICS_BEGIN__\t9\nCPU\t1\t1\n"
            "__NOVATERM_METRICS_END__\t10\n"));
        if (result.error == SshMonitorFrameParser::Error::None)
            ++failures;

        QByteArray tooMany("__NOVATERM_METRICS_BEGIN__\t11\n");
        for (int i = 0; i <= SshMonitorFrameParser::MaxFrameEntries; ++i)
            tooMany.append("NET\teth0\t1\t2\n");
        result = parser.append(tooMany);
        if (result.error == SshMonitorFrameParser::Error::None)
            ++failures;

        QByteArray oversizedFrame("__NOVATERM_METRICS_BEGIN__\t12\n");
        for (int i = 0; i < 10; ++i)
            oversizedFrame.append(QByteArray(14 * 1024, 'x') + '\n');
        result = parser.append(oversizedFrame);
        if (result.error == SshMonitorFrameParser::Error::None)
            ++failures;

        result = parser.append(QByteArray(
            SshMonitorFrameParser::MaxBufferedBytes + 1, 'x'));
        if (result.error == SshMonitorFrameParser::Error::None)
            ++failures;
        std::printf("[monitor-protocol] fragmentation/multiple/invalid/limits checked\n");
    }

    // ── 用例 1：无效配置 → 同步失败 ─────────────────────────
    {
        SshConfig cfg;   // host / username 均为空
        SshTransport transport(cfg);
        // 未连接时不得接受资源监控等辅助命令，避免请求滞留到下一代连接。
        if (transport.executeCommand(1, QByteArrayLiteral("true")))
            ++failures;
        transport.startResourceMonitoring();
        if (transport.requestResourceSample(2))
            ++failures;
        const bool ok = transport.connectToHost();
        std::printf("[invalid-config] connectToHost=%d error='%s'\n",
                    static_cast<int>(ok),
                    transport.errorString().toUtf8().constData());
        if (ok)
            ++failures;
    }

    // ── 用例 2：连接被拒 → 异步 errorOccurred + 线程回收 ─────
    {
        SshConfig cfg;
        cfg.host = QStringLiteral("127.0.0.1");
        cfg.port = 1;          // 无服务监听 → 立即 connection refused
        cfg.username = QStringLiteral("test");
        cfg.password = QStringLiteral("test");

        SshTransport transport(cfg);
        QObject::connect(&transport, &SshTransport::errorOccurred,
                         [&](const QString& error) {
            std::printf("[refused] errorOccurred='%s'\n",
                        error.toUtf8().constData());
            if (error.isEmpty())
                ++failures;
            QTimer::singleShot(0, &app, &QCoreApplication::quit);
        });
        QObject::connect(&transport, &SshTransport::connected, [&]() {
            std::printf("[refused] UNEXPECTED connected\n");
            ++failures;
            QTimer::singleShot(0, &app, &QCoreApplication::quit);
        });
        QObject::connect(&transport, &SshTransport::disconnected, [&]() {
            std::printf("[refused] UNEXPECTED disconnected\n");
            ++failures;
        });

        const bool ok = transport.connectToHost();
        std::printf("[refused] connectToHost=%d\n", static_cast<int>(ok));
        if (!ok) {
            ++failures;
            return 1;
        }

        QTimer::singleShot(15000, &app, [&]() {
            std::printf("[refused] TIMEOUT: no error within 15s\n");
            ++failures;
            app.quit();
        });
        app.exec();

        // 错误应已触发；此时析构会回收工作线程（不崩溃即通过）。
        std::printf("[refused] teardown ok, errorString='%s'\n",
                    transport.errorString().toUtf8().constData());
    }

    std::printf(failures == 0 ? "RESULT: PASS\n" : "RESULT: FAIL (%d)\n",
                failures);
    return failures == 0 ? 0 : 1;
}
