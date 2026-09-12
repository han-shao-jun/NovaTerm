/**
 * @file SshMonitorIntegrationCheck.cpp
 * @brief 使用历史 SSH 会话执行资源监控人工集成检查。
 *
 * 此程序不注册到 ctest，避免开发机无对应服务器时产生失败。它只按标题匹配
 * “zynq”，凭据通过 CredentialStore 读取且绝不输出。
 */
#include "credential/CredentialStore.h"
#include "session/SessionStore.h"
#include "transport/SshTransport.h"
#include "service/LinuxResourceData.h"
#include "service/ResourcePrefetchSchedule.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <cstdio>
#include <memory>
#include <optional>
#include <utility>

namespace {

std::optional<SshConfig> loadZynqConfig()
{
    const QDir dataDirectory(QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation));
    SessionStore store(dataDirectory.filePath(
        QStringLiteral("session-history.json")));
    const auto credentials = createCredentialStore();
    const QList<SessionRestoreMetadata> entries = store.load();
    int sshEntryCount = 0;
    for (const SessionRestoreMetadata& entry : entries) {
        if (entry.runtimeSnapshot.transportKind == TransportKind::Ssh)
            ++sshEntryCount;
    }
    for (const SessionRestoreMetadata& entry : entries) {
        const RuntimeConfig& runtime = entry.runtimeSnapshot;
        if (runtime.transportKind != TransportKind::Ssh
            || (sshEntryCount != 1
                && !runtime.title.contains(QStringLiteral("zynq"),
                                           Qt::CaseInsensitive))) {
            continue;
        }

        const QVariantMap& values = runtime.transport;
        SshConfig config;
        config.host = values.value(QStringLiteral("host")).toString();
        config.username = values.value(QStringLiteral("username")).toString();
        config.port = static_cast<quint16>(values.value(
            QStringLiteral("port"), 22).toUInt());
        config.authMethod = values.value(
            QStringLiteral("authMethod"),
            QStringLiteral("password")).toString();
        config.privateKeyPath = values.value(
            QStringLiteral("privateKeyPath")).toString();
        config.terminalType = values.value(
            QStringLiteral("terminalType"),
            QStringLiteral("xterm-256color")).toString();
        config.keepAliveSeconds = values.value(
            QStringLiteral("keepAliveSeconds"), 30).toInt();
        config.label = runtime.title;
        if (!runtime.credentialRef.isEmpty()) {
            const auto secret = credentials->get(runtime.credentialRef);
            if (!secret)
                return std::nullopt;
            if (config.authMethod == QStringLiteral("password"))
                config.password = QString::fromUtf8(*secret);
            else
                config.keyPassphrase = QString::fromUtf8(*secret);
        }
        return config.isValid() ? std::optional<SshConfig>{std::move(config)}
                                : std::nullopt;
    }
    return std::nullopt;
}

} // namespace

namespace {

/** 同步执行一条远端命令；单次命令通道本身就是单请求，天然串行。 */
bool runOneCommand(SshTransport& transport, const QByteArray& command,
                   QByteArray& output, QString& error)
{
    static quint64 nextId = 10'000;
    const quint64 id = nextId++;
    QEventLoop loop;
    QByteArray result;
    QString failure;
    bool done = false;
    const auto connection = QObject::connect(
        &transport, &SshTransport::commandFinished, &loop,
        [&](quint64 requestId, const QByteArray& standardOutput,
            const QByteArray&, const QString& errorMessage) {
            if (requestId != id)
                return;
            result = standardOutput;
            failure = errorMessage;
            done = true;
            loop.quit();
        });
    QTimer guard;
    guard.setSingleShot(true);
    guard.setInterval(30'000);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start();
    if (!transport.executeCommand(id, command)) {
        QObject::disconnect(connection);
        return false;
    }
    loop.exec();
    QObject::disconnect(connection);
    output = result;
    error = failure;
    return done;
}

/** `/proc/stat` 快照：聚合行累计 tick 与核心数。 */
struct CpuSnapshot
{
    quint64 total{0};
    quint64 idle{0};
    int cores{0};
};

CpuSnapshot parseCpuStat(const QByteArray& statText)
{
    CpuSnapshot snapshot;
    for (const QByteArray& rawLine : statText.split('\n')) {
        const QList<QByteArray> fields = rawLine.simplified().split(' ');
        if (fields.isEmpty() || !fields[0].startsWith("cpu"))
            continue;
        if (fields[0] != "cpu") {
            ++snapshot.cores;
            continue;
        }
        for (int index = 1; index < fields.size() && index <= 8; ++index) {
            bool ok = false;
            const quint64 value = fields[index].toULongLong(&ok);
            if (!ok)
                continue;
            snapshot.total += value;
            if (index == 4 || index == 5)   // idle + iowait
                snapshot.idle += value;
        }
    }
    if (snapshot.cores == 0)
        snapshot.cores = 1;
    return snapshot;
}

/** 区间内整机 CPU 占用率（与面板同口径：1 - idle/total）。 */
double busyPercent(const CpuSnapshot& before, const CpuSnapshot& after)
{
    if (after.total <= before.total)
        return 0.0;
    const quint64 total = after.total - before.total;
    const quint64 idle = after.idle >= before.idle ? after.idle - before.idle : 0;
    return 100.0 * double(total - std::min(idle, total)) / double(total);
}

/** 连接历史会话；失败返回 false。 */
bool connectTransport(SshTransport& transport, QCoreApplication& app,
                      QString& error)
{
    bool connected = false;
    QEventLoop loop;
    QObject::connect(&transport, &ITransport::connected, &loop, [&]() {
        connected = true;
        loop.quit();
    });
    QObject::connect(&transport, &ITransport::errorOccurred, &loop,
                     [&](const QString& message) {
        error = message;
        loop.quit();
    });
    QObject::connect(&transport, &SshTransport::hostKeyRequired, &loop,
                     [&](const SshHostKeyInfo&) { transport.rejectHostKey(); });
    QTimer guard;
    guard.setSingleShot(true);
    guard.setInterval(20'000);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start();
    static_cast<void>(app);
    if (!transport.connectToHost()) {
        error = QStringLiteral("connectToHost refused");
        return false;
    }
    loop.exec();
    return connected;
}

/**
 * @brief 逐命令实测：每条命令重复若干次，测整机 CPU 占用率与单次 CPU·ms。
 *
 * 用「重复 + /proc/stat 前后差」而不是 `times`，避免依赖远端 shell 内建差异；
 * 空闲机上重复窗口内我方开销占主导，因此可比较各命令的相对成本。
 */
int profileCommands(QCoreApplication& app)
{
    auto config = loadZynqConfig();
    if (!config) {
        std::fprintf(stderr, "Zynq SSH history or credential was not found.\n");
        return 2;
    }
    SshTransport transport(std::move(*config));
    QString error;
    if (!connectTransport(transport, app, error)) {
        std::fprintf(stderr, "connect failed: %s\n", error.toUtf8().constData());
        return 1;
    }

    struct Case
    {
        const char* name;
        QByteArray command;
    };
    const QList<Case> cases{
        {"monitor-frame(one-shot)",
         QByteArrayLiteral("IFS= read -r c < /proc/stat\n"
                           "cat /proc/meminfo /proc/net/dev\n"
                           "IFS= read -r v < /proc/loadavg\n"
                           "IFS= read -r w < /proc/uptime\n")},
        {"df -Pk", QByteArrayLiteral("df -Pk\n")},
        {"slowCommand(freq+df)", NovaTerm::LinuxResource::slowCommand()},
        {"slowCommand(df-only)",
         NovaTerm::LinuxResource::slowCommand(/*includeFrequency=*/false)},
        {"static batch0 overview+ip",
         NovaTerm::LinuxResource::staticCommand(0)},
        {"static batch1 cpuinfo+freq",
         NovaTerm::LinuxResource::staticCommand(1)},
        {"static batch2 gpu(lspci)",
         NovaTerm::LinuxResource::staticCommand(2)},
    };

    constexpr int Repeats = 8;
    std::printf("%-24s %9s %8s %10s %12s\n", "command", "wall_ms",
                "busy_pct", "cpu_ms/run", "cores");
    for (const Case& one : cases) {
        const QByteArray wrapper =
            QByteArrayLiteral("LC_ALL=C; export LC_ALL\n")
            // 核数在测量窗口之外读，避免把 grep 的进程成本算进被测命令。
            + QByteArrayLiteral("nt_cores=$(grep -c '^cpu[0-9]' /proc/stat 2>/dev/null)\n")
            + QByteArrayLiteral("IFS= read -r nt_a < /proc/stat\n")
            + QByteArrayLiteral("i=0\nwhile [ $i -lt ")
            + QByteArray::number(Repeats)
            + QByteArrayLiteral(" ]; do\n") + one.command
            + QByteArrayLiteral(" >/dev/null 2>&1\ni=$((i+1))\ndone\n")
            + QByteArrayLiteral("IFS= read -r nt_b < /proc/stat\n")
            + QByteArrayLiteral(
                "printf 'NVA\\t%s\\nNVB\\t%s\\nNVC\\t%s\\n' \"$nt_a\" \"$nt_b\" \"$nt_cores\"\n");
        QByteArray output;
        QString commandError;
        QElapsedTimer wall;
        wall.start();
        if (!runOneCommand(transport, wrapper, output, commandError)) {
            std::fprintf(stderr, "command failed: %s (%s)\n", one.name,
                         commandError.toUtf8().constData());
            continue;
        }
        const qint64 wallMs = wall.elapsed();
        QByteArray lineA;
        QByteArray lineB;
        int cores = 0;
        for (const QByteArray& line : output.split('\n')) {
            if (line.startsWith("NVA\t"))
                lineA = line.mid(4);
            else if (line.startsWith("NVB\t"))
                lineB = line.mid(4);
            else if (line.startsWith("NVC\t"))
                cores = line.mid(4).trimmed().toInt();
        }
        const CpuSnapshot before = parseCpuStat(lineA);
        const CpuSnapshot after = parseCpuStat(lineB);
        if (cores <= 0)
            cores = after.cores;
        const double busy = busyPercent(before, after);
        // 窗口内 CPU 容量 = wall × 核数；乘以占用率再除以重复次数 = 单次 CPU·ms。
        const double cpuMsPerRun = busy / 100.0 * double(wallMs)
            * double(cores) / double(Repeats);
        std::printf("%-24s %9lld %8.1f %10.2f %12d\n", one.name,
                    static_cast<long long>(wallMs), busy, cpuMsPerRun, cores);
        std::fflush(stdout);
    }

    transport.disconnect();
    return 0;
}

/**
 * @brief 复现连接后的采集序列：先测空闲基线，再测首次采集突发。
 *
 * 目的：判断面板显示的"首次采集 CPU 10%"是真实负载、还是我方命令叠加（或读数
 * 假象）。只用常驻通道 1 Hz 采样整机 /proc/stat（便宜且与面板同口径），期间在
 * 单次命令通道上按策略发出 df 与详情批次，逐秒打印占用率：
 *
 *   阶段 A（空闲基线）：只跑常驻采样，确认设备本底占用；
 *   阶段 B（首次采集）：按 --defer-df / --batch-start / --batch-gap 发命令。
 *
 * 用法示例：--profile-collection [--defer-df=1500 --batch-start=2000 --batch-gap=800]
 */
int profileCollection(QCoreApplication& app)
{
    auto config = loadZynqConfig();
    if (!config) {
        std::fprintf(stderr, "Zynq SSH history or credential was not found.\n");
        return 2;
    }
    SshTransport transport(std::move(*config));

    int deferDfMs = 0;
    int batchStartMs = 400;
    int batchGapMs = 800;
    bool immediateBurst = false;
    for (const QString& argument : app.arguments()) {
        if (argument.startsWith(QStringLiteral("--defer-df=")))
            deferDfMs = argument.section('=', 1).toInt();
        else if (argument.startsWith(QStringLiteral("--batch-start=")))
            batchStartMs = argument.section('=', 1).toInt();
        else if (argument.startsWith(QStringLiteral("--batch-gap=")))
            batchGapMs = argument.section('=', 1).toInt();
        else if (argument == QStringLiteral("--no-baseline"))
            immediateBurst = true;   // 复现"连接即采集"：不做空闲基线，直接计时
    }

    QString error;
    if (!connectTransport(transport, app, error)) {
        std::fprintf(stderr, "connect failed: %s\n", error.toUtf8().constData());
        return 1;
    }

    transport.startResourceMonitoring();

    // 先确认设备核数：CPU 占用率是"整机平均利用率"，核数既决定单条命令的实际
    // CPU 成本换算，也决定 1 秒区间内 1% 对应多少 tick。
    {
        QByteArray output;
        QString commandError;
        runOneCommand(transport,
                      QByteArrayLiteral(
                          "printf 'cores='; grep -c '^cpu[0-9]' /proc/stat 2>/dev/null\n"
                          "grep -m1 'model name' /proc/cpuinfo 2>/dev/null\n"
                          "grep -m1 '^Processor' /proc/cpuinfo 2>/dev/null\n"
                          "printf 'nproc='; nproc 2>/dev/null\n"),
                      output, commandError);
        std::printf("device: %s\n", output.simplified().constData());
        std::fflush(stdout);
    }

    constexpr int BaselineFrames = 3;    // 阶段 A：3 个 1 秒区间
    constexpr int BurstFrames = 5;       // 阶段 B：覆盖 4 个批次 + 余量
    constexpr int TotalFrames = BaselineFrames + BurstFrames + 1;

    int frames = 0;
    bool burstPhase = immediateBurst;
    QByteArray pendingStat;
    quint64 pendingId = 0;
    quint64 nextId = 100;
    double baselineSum = 0.0;
    int baselineIntervals = 0;
    double burstSum = 0.0;
    double burstPeak = 0.0;
    int burstIntervals = 0;
    QElapsedTimer clock;
    clock.start();

    const auto requestFrame = [&]() {
        if (pendingId != 0)
            return;
        const quint64 id = nextId++;
        if (transport.requestResourceSample(id))
            pendingId = id;
    };

    const auto issue = [&](const char* label, const QByteArray& command) {
        QByteArray output;
        QString commandError;
        QElapsedTimer commandClock;
        commandClock.start();
        runOneCommand(transport, command, output, commandError);
        std::printf("   +%5lldms  issue %-22s wall=%lldms%s\n",
                    static_cast<long long>(clock.elapsed()), label,
                    static_cast<long long>(commandClock.elapsed()),
                    commandError.isEmpty() ? "" : " (error)");
        std::fflush(stdout);
    };

    const auto startSequence = [&]() {
        std::printf("--- 开始采集序列（deferDf=%dms batchStart=%dms gap=%dms）---\n",
                    deferDfMs, batchStartMs, batchGapMs);
        std::fflush(stdout);
        QTimer::singleShot(deferDfMs, &app, [&]() {
            issue("df-only", NovaTerm::LinuxResource::slowCommand(
                                 /*includeFrequency=*/false));
        });
        for (int batch = 0;
             batch < NovaTerm::LinuxResource::PrefetchSchedule::BatchCount;
             ++batch) {
            const int delay = batchStartMs + batch * batchGapMs;
            QTimer::singleShot(delay, &app, [&, batch]() {
                issue(batch == 0 ? "static batch0 overview+ip"
                                 : batch == 1 ? "static batch1 cpuinfo"
                                              : "static batch2 gpu",
                      NovaTerm::LinuxResource::staticCommand(batch));
            });
        }
    };

    QObject::connect(&transport, &SshTransport::resourceSampleFinished, &app,
                     [&](quint64 requestId, const QByteArray& payload,
                         const QString& frameError) {
        if (requestId != pendingId || !frameError.isEmpty())
            return;
        pendingId = 0;
        // 取 @@stat 分节的首行（聚合 cpu 行）用于本地差分。
        QByteArray stat;
        const int statStart = payload.indexOf("@@stat\n");
        if (statStart >= 0) {
            const int lineStart = statStart + 7;
            int lineEnd = payload.indexOf('\n', lineStart);
            if (lineEnd < 0)
                lineEnd = payload.size();
            stat = payload.mid(lineStart, lineEnd - lineStart);
        }
        const CpuSnapshot current = parseCpuStat(stat);
        if (!pendingStat.isEmpty()) {
            const double busy = busyPercent(parseCpuStat(pendingStat), current);
            if (burstPhase) {
                burstSum += busy;
                burstPeak = std::max(burstPeak, busy);
                ++burstIntervals;
            } else {
                baselineSum += busy;
                ++baselineIntervals;
            }
            std::printf("t=%5lldms  %-8s cpu=%5.1f%%\n",
                        static_cast<long long>(clock.elapsed()),
                        burstPhase ? "burst" : "baseline", busy);
            std::fflush(stdout);
        }
        pendingStat = stat;
        ++frames;
        if (frames >= TotalFrames) {
            app.quit();
            return;
        }
        if (!burstPhase && frames >= BaselineFrames) {
            // 空闲基线够了，进入突发阶段；命令时间轴以此为 t0。
            burstPhase = true;
            startSequence();
        }
        QTimer::singleShot(1000, &app, requestFrame);
    });

    QTimer::singleShot(0, &app, [&]() { requestFrame(); });

    if (immediateBurst)
        startSequence();   // 时间轴 t0 = 连接完成，复现应用的真实首秒

    QTimer deadline;
    deadline.setSingleShot(true);
    deadline.setInterval(30'000);
    QObject::connect(&deadline, &QTimer::timeout, &app, [&]() { app.quit(); });
    deadline.start();

    app.exec();

    std::printf("plan: deferDf=%dms batchStart=%dms gap=%dms\n", deferDfMs,
                batchStartMs, batchGapMs);
    std::printf("baseline: avg=%.1f%% (%d intervals)\n",
                baselineIntervals ? baselineSum / baselineIntervals : 0.0,
                baselineIntervals);
    std::printf("burst:    peak=%.1f%% avg=%.1f%% (%d intervals)\n", burstPeak,
                burstIntervals ? burstSum / burstIntervals : 0.0,
                burstIntervals);
    transport.stopResourceMonitoring();
    transport.disconnect();
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("NovaTerm"));
    app.setOrganizationName(QStringLiteral("NovaTerm"));
    const bool probeFileSystems = app.arguments().contains(
        QStringLiteral("--df-only"));
    // 采集策略实测模式（见上方 profileCommands / profileCollection 注释）。
    if (app.arguments().contains(QStringLiteral("--profile-commands")))
        return profileCommands(app);
    if (app.arguments().contains(QStringLiteral("--profile-collection")))
        return profileCollection(app);
    auto config = loadZynqConfig();
    if (!config) {
        std::fprintf(stderr, "Zynq SSH history or credential was not found.\n");
        return 2;
    }

    SshTransport transport(std::move(*config));
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer requestTimer;
    requestTimer.setInterval(1000);
    QTimer deadline;
    deadline.setSingleShot(true);
    deadline.setInterval(30000);

    int phase = 0;
    int samples = 0;
    int samplesWhileSlowCommand = 0;
    int lateSamples = 0;
    bool slowCommandActive = false;
    bool slowCommandTimedOut = false;
    bool terminalIoOk = false;
    bool monitorStopped = false;
    bool remoteProcessGone = false;
    bool reconnectSampleOk = false;
    bool fileSystemProbeOk = false;
    quint64 pendingSample = 0;
    quint64 nextSample = 100;
    QByteArray terminalOutput;

    const auto requestSample = [&]() {
        if (pendingSample != 0 || monitorStopped)
            return;
        const quint64 id = nextSample++;
        if (transport.requestResourceSample(id))
            pendingSample = id;
    };

    QObject::connect(&requestTimer, &QTimer::timeout, &app, requestSample);
    QObject::connect(&deadline, &QTimer::timeout, &app, [&]() {
        std::fprintf(stderr, "Integration check timed out.\n");
        app.exit(1);
    });
    QObject::connect(&transport, &SshTransport::hostKeyRequired, &app,
                     [&](const SshHostKeyInfo&) {
        // 历史会话应已信任。密钥变化不在测试程序中静默接受。
        transport.rejectHostKey();
    });
    QObject::connect(&transport, &ITransport::errorOccurred, &app,
                     [&](const QString& error) {
        std::fprintf(stderr, "SSH error: %s\n",
                     error.toUtf8().constData());
    });
    QObject::connect(&transport, &ITransport::readyRead, &app,
                     [&](const QByteArray& data) {
        terminalOutput.append(data);
        if (terminalOutput.size() > 128 * 1024)
            terminalOutput.remove(0, terminalOutput.size() - 128 * 1024);
        terminalIoOk |= terminalOutput.contains("__NOVATERM_IO_OK__");
    });
    QObject::connect(
        &transport, &SshTransport::resourceSampleFinished, &app,
        [&](quint64 requestId, const QByteArray& payload,
            const QString& error) {
        if (monitorStopped) {
            ++lateSamples;
            return;
        }
        NovaTerm::LinuxResource::Sample sample;
        if (requestId != pendingSample || !error.isEmpty()
            || !NovaTerm::LinuxResource::parseMetrics(payload, sample)) {
            std::fprintf(stderr, "Invalid resource frame: %s\n",
                         error.toUtf8().constData());
            app.exit(1);
            return;
        }
        pendingSample = 0;
        ++samples;
        if (slowCommandActive)
            ++samplesWhileSlowCommand;
        if (phase == 1) {
            reconnectSampleOk = true;
            transport.stopResourceMonitoring();
            transport.disconnect();
            app.quit();
        }
    });
    QObject::connect(
        &transport, &SshTransport::commandFinished, &app,
        [&](quint64 requestId, const QByteArray& output,
            const QByteArray& errorOutput, const QString& error) {
        if (requestId == 8999) {
            std::printf("df_stdout_bytes=%lld df_stderr_bytes=%lld error=%s\n",
                        static_cast<long long>(output.size()),
                        static_cast<long long>(errorOutput.size()),
                        error.toUtf8().constData());
            fileSystemProbeOk = error.isEmpty() && output.contains("FS\t");
            transport.disconnect();
            app.quit();
            return;
        }
        if (requestId == 9001) {
            slowCommandActive = false;
            slowCommandTimedOut = error.contains(
                QStringLiteral("timed out"), Qt::CaseInsensitive);
            return;
        }
        if (requestId != 9002)
            return;
        const QRegularExpressionMatch match = QRegularExpression(
            QStringLiteral("MONPROC\\s+(\\d+)")).match(
                QString::fromUtf8(output));
        remoteProcessGone = match.hasMatch()
            && match.captured(1).toInt() == 0;
        transport.disconnect();
        if (remoteProcessGone) {
            phase = 1;
            monitorStopped = false;
            pendingSample = 0;
            QTimer::singleShot(100, &app, [&]() {
                static_cast<void>(transport.connectToHost());
            });
        }
    });
    QObject::connect(&transport, &ITransport::connected, &app, [&]() {
        transport.startResourceMonitoring();
        if (probeFileSystems) {
            static_cast<void>(transport.executeCommand(
                8999, QByteArrayLiteral(
                    "LC_ALL=C; export LC_ALL; "
                    "(df -Pk 2>/dev/null || df -k 2>/dev/null) | "
                    "awk 'NR > 1 { printf \"FS\\t%s\\t%s\\t%s\\n\", "
                    "$NF, $4, $2 }'")));
            return;
        }
        if (phase == 1) {
            requestSample();
            return;
        }

        requestTimer.start();
        requestSample();
        transport.write(QByteArrayLiteral(
            "printf '__NOVATERM_IO_OK__\\n'\r"));
        QTimer::singleShot(500, &app, [&]() {
            slowCommandActive = transport.executeCommand(
                9001, QByteArrayLiteral("sleep 7; df -Pk / >/dev/null"));
        });
        QTimer::singleShot(8500, &app, [&]() {
            requestTimer.stop();
            monitorStopped = true;
            pendingSample = 0;
            transport.stopResourceMonitoring();
            QTimer::singleShot(1000, &app, [&]() {
                static_cast<void>(transport.executeCommand(
                    9002, QByteArrayLiteral(
                        "ps -eo args | awk '/[N]OVATERM_METRICS_BEGIN__/ "
                        "{n++} END {printf \"MONPROC\\t%d\\n\", n+0}'")));
            });
        });
    });

    deadline.start();
    if (!transport.connectToHost())
        return 1;
    app.exec();

    if (probeFileSystems)
        return fileSystemProbeOk ? 0 : 1;

    const bool passed = samples >= 3 && samplesWhileSlowCommand >= 2
        && slowCommandTimedOut && terminalIoOk && remoteProcessGone
        && reconnectSampleOk && lateSamples == 0;
    std::printf(
        "samples=%d during_slow=%d slow_timeout=%d terminal_io=%d "
        "process_gone=%d reconnect=%d late=%d elapsed_ms=%lld\n",
        samples, samplesWhileSlowCommand, slowCommandTimedOut, terminalIoOk,
        remoteProcessGone, reconnectSampleOk, lateSamples,
        static_cast<long long>(elapsed.elapsed()));
    return passed ? 0 : 1;
}
