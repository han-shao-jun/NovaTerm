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

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
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

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("NovaTerm"));
    app.setOrganizationName(QStringLiteral("NovaTerm"));
    const bool probeFileSystems = app.arguments().contains(
        QStringLiteral("--df-only"));
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
