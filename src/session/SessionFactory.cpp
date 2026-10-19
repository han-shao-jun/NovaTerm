/**
 * @file   SessionFactory.cpp
 * @brief  会话工厂实现：profile 解析与会话装配。
 *
 * resolve() 校验 profile ID 与凭据引用方式（禁止嵌入凭据）。
 * create() 按 transportKind 从 QVariantMap 反序列化为具体 Config，
 * SSH 凭据通过 CredentialStore 按 credentialRef 解析。
 * createLocal/Serial/Ssh/Telnet 是直接装配入口，绕过 profile 层。
 */
#include "SessionFactory.h"

#include "TerminalSession.h"
#include "core/terminal/TerminalCore.h"
#include "credential/CredentialStore.h"
#include "profile/ProfileStore.h"
#include "ShellIntegration.h"
#include "transport/LocalShellTransport.h"
#include "transport/SerialTransport.h"
#include "transport/SshTransport.h"
#include "transport/TelnetTransport.h"

namespace {

template <typename Transport, typename Config>
std::unique_ptr<TerminalSession> makeSession(Config config, RuntimeConfig runtime,
                                             TransportKind kind, SessionHistoryConfig history)
{
    runtime.transportKind = kind;
    auto session = std::make_unique<TerminalSession>(std::move(runtime));
    static_cast<void>(session->core()->configureHistory(history.runtime, history.policy));
    session->attach(new Transport(std::move(config)), TerminalSession::Ownership::Adopt);
    return session;
}

} // namespace

std::optional<RuntimeConfig>
SessionFactory::resolve(const ConnectionProfile& profile,
                        const QVariantMap& overrides, QString* error)
{
    if (profile.id.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("profile id is empty");
        return std::nullopt;
    }
    if (ProfileStore::containsSensitiveValues(profile.settings)
        || ProfileStore::containsSensitiveValues(overrides)) {
        if (error)
            *error = QStringLiteral("credentials must be referenced, not embedded");
        return std::nullopt;
    }
    RuntimeConfig runtime;
    runtime.profileId = profile.id;
    runtime.credentialRef = profile.credentialRef;
    runtime.title = profile.name;
    runtime.transportKind = profile.transportKind;
    runtime.transport = profile.settings;
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it)
        runtime.transport.insert(it.key(), it.value());
    return runtime;
}

std::unique_ptr<TerminalSession>
SessionFactory::create(const RuntimeConfig& runtime,
                       const CredentialStore* credentials, QString* error, SessionHistoryConfig history)
{
    const QVariantMap& values = runtime.transport;
    switch (runtime.transportKind) {
    case TransportKind::LocalShell: {
        LocalShellConfig config;
        config.profile.name = runtime.title.isEmpty()
            ? QStringLiteral("Local shell") : runtime.title;
        config.profile.executable = values.value(QStringLiteral("executable")).toString();
        config.profile.arguments = values.value(QStringLiteral("arguments")).toStringList();
        config.profile.workingDirectory = values.value(
            QStringLiteral("workingDirectory")).toString();
        if (!config.isValid()) {
            if (error)
                *error = QStringLiteral("invalid local shell runtime config");
            return nullptr;
        }
        return createLocal(config, runtime, history);
    }
    case TransportKind::Serial: {
        SerialConfig config;
        config.portName = values.value(QStringLiteral("portName")).toString();
        config.baudRate = values.value(QStringLiteral("baudRate"), 115200).toInt();
        config.dataBits = static_cast<QSerialPort::DataBits>(
            values.value(QStringLiteral("dataBits"), QSerialPort::Data8).toInt());
        config.parity = static_cast<QSerialPort::Parity>(
            values.value(QStringLiteral("parity"), QSerialPort::NoParity).toInt());
        config.stopBits = static_cast<QSerialPort::StopBits>(
            values.value(QStringLiteral("stopBits"), QSerialPort::OneStop).toInt());
        config.flowControl = static_cast<QSerialPort::FlowControl>(
            values.value(QStringLiteral("flowControl"),
                         QSerialPort::NoFlowControl).toInt());
        config.reconnectSeconds = values.value(QStringLiteral("reconnectSeconds"), 0).toInt();
        config.lfImpliesCr = values.value(QStringLiteral("lfImpliesCr"), false).toBool();
        config.interactivePromptPattern = values.value(QStringLiteral("interactivePromptPattern")).toString();
        config.interactiveLineEnding = values.value(QStringLiteral("interactiveLineEnding"), QStringLiteral("cr")).toString();
        config.interactiveRemoteEcho = values.value(QStringLiteral("interactiveRemoteEcho"), true).toBool();
        config.label = runtime.title;
        if (!config.isValid()) {
            if (error)
                *error = QStringLiteral("invalid serial runtime config");
            return nullptr;
        }
        return createSerial(config, runtime, history);
    }
    case TransportKind::Ssh: {
        SshConfig config;
        config.host = values.value(QStringLiteral("host")).toString();
        config.username = values.value(QStringLiteral("username")).toString();
        config.port = static_cast<quint16>(values.value(
            QStringLiteral("port"), 22).toUInt());
        config.authMethod = values.value(
            QStringLiteral("authMethod"), QStringLiteral("password")).toString();
        config.privateKeyPath = values.value(
            QStringLiteral("privateKeyPath")).toString();
        config.terminalType = values.value(
            QStringLiteral("terminalType"),
            QStringLiteral("xterm-256color")).toString();
        config.keepAliveSeconds = values.value(
            QStringLiteral("keepAliveSeconds"), 30).toInt();
        config.label = runtime.title;
        config.interactiveShellKind = values.value(
            QStringLiteral("interactiveShellKind")).toString();
        config.environment = ShellIntegration::startupEnvironmentFor(runtime);
        if (!runtime.credentialRef.isEmpty()) {
            if (!credentials) {
                if (error)
                    *error = QStringLiteral("credential store is required");
                return nullptr;
            }
            const auto secret = credentials->get(runtime.credentialRef);
            if (!secret) {
                if (error)
                    *error = QStringLiteral("credential reference was not found");
                return nullptr;
            }
            if (config.authMethod == QStringLiteral("password"))
                config.password = QString::fromUtf8(*secret);
            else
                config.keyPassphrase = QString::fromUtf8(*secret);
        }
        if (!config.isValid()) {
            if (error)
                *error = QStringLiteral("invalid SSH runtime config");
            return nullptr;
        }
        return createSsh(config, runtime, history);
    }
    case TransportKind::Telnet: {
        TelnetConfig config;
        config.host = values.value(QStringLiteral("host")).toString();
        config.port = static_cast<quint16>(values.value(
            QStringLiteral("port"), 23).toUInt());
        config.terminalType = values.value(
            QStringLiteral("terminalType"),
            QStringLiteral("xterm-256color")).toString();
        config.naws = values.value(QStringLiteral("naws"), true).toBool();
        config.binaryMode = values.value(
            QStringLiteral("binaryMode"), false).toBool();
        config.keepAliveSeconds = values.value(
            QStringLiteral("keepAliveSeconds"), 0).toInt();
        config.label = runtime.title;
        if (!config.isValid()) {
            if (error)
                *error = QStringLiteral("invalid Telnet runtime config");
            return nullptr;
        }
        return createTelnet(config, runtime, history);
    }
    case TransportKind::Custom:
        if (error)
            *error = QStringLiteral("custom transport requires an external factory");
        return nullptr;
    }
    return nullptr;
}

std::unique_ptr<TerminalSession>
SessionFactory::createLocal(const LocalShellConfig& config, RuntimeConfig runtime, SessionHistoryConfig history)
{
    runtime.transportKind = TransportKind::LocalShell;
    if (!config.profile.interactiveShellKind.isEmpty()) {
        runtime.transport.insert(QStringLiteral("interactiveShellKind"),
                                 config.profile.interactiveShellKind);
        if (config.profile.interactiveShellKind == QStringLiteral("cmd")) {
            runtime.transport.insert(QStringLiteral("interactiveHookReady"),
                config.profile.environment.contains(QStringLiteral("CLINK_PATH")));
        }
    }
    auto session = std::make_unique<TerminalSession>(std::move(runtime));
    static_cast<void>(session->core()->configureHistory(history.runtime, history.policy));
    auto* transport = new LocalShellTransport;
    transport->setSessionConfig(config);
    session->attach(transport, TerminalSession::Ownership::Adopt);
    return session;
}

std::unique_ptr<TerminalSession>
SessionFactory::createSerial(const SerialConfig& config, RuntimeConfig runtime, SessionHistoryConfig history)
{
    if (!config.isValid())
        return nullptr;
    runtime.transport.insert(QStringLiteral("reconnectSeconds"), config.reconnectSeconds);
    runtime.transport.insert(QStringLiteral("lfImpliesCr"), config.lfImpliesCr);
    runtime.transport.insert(QStringLiteral("portName"), config.portName);
    runtime.transport.insert(QStringLiteral("interactivePromptPattern"), config.interactivePromptPattern);
    runtime.transport.insert(QStringLiteral("interactiveLineEnding"), config.interactiveLineEnding);
    runtime.transport.insert(QStringLiteral("interactiveRemoteEcho"), config.interactiveRemoteEcho);
    auto session = makeSession<SerialTransport>(config, std::move(runtime),
                                               TransportKind::Serial, std::move(history));
    if (!session->core()->setLfImpliesCr(config.lfImpliesCr))
        return nullptr;
    return session;
}

std::unique_ptr<TerminalSession>
SessionFactory::createSsh(const SshConfig& config, RuntimeConfig runtime, SessionHistoryConfig history)
{
    auto configured = config;
    if (!configured.interactiveShellKind.isEmpty()) {
        runtime.transport.insert(QStringLiteral("interactiveShellKind"),
                                 configured.interactiveShellKind);
        configured.environment = ShellIntegration::startupEnvironmentFor(runtime);
    }
    return makeSession<SshTransport>(configured, std::move(runtime), TransportKind::Ssh, std::move(history));
}

std::unique_ptr<TerminalSession>
SessionFactory::createTelnet(const TelnetConfig& config, RuntimeConfig runtime, SessionHistoryConfig history)
{
    return makeSession<TelnetTransport>(config, std::move(runtime),
                                        TransportKind::Telnet, std::move(history));
}
