/**
 * @file   SessionFactory.cpp
 * @brief  会话工厂实现：profile 解析与会话装配。
 *
 * resolve() 校验 profile ID 与凭据引用方式（禁止嵌入凭据）。
 * create() 按 transportKind 从 QVariantMap 反序列化为具体 Config，
 * SSH 凭据通过 CredentialStore 按 credentialRef 解析。
 * createLocal/Serial/Ssh 是直接装配入口，绕过 profile 层。
 */
#include "SessionFactory.h"

#include "TerminalSession.h"
#include "credential/CredentialStore.h"
#include "profile/ProfileStore.h"
#include "transport/LocalShellTransport.h"
#include "transport/SerialTransport.h"
#include "transport/SshTransport.h"

namespace {

template <typename Transport, typename Config>
std::unique_ptr<TerminalSession> makeSession(Config config, RuntimeConfig runtime,
                                             TransportKind kind)
{
    runtime.transportKind = kind;
    auto session = std::make_unique<TerminalSession>(std::move(runtime));
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
                       const CredentialStore* credentials, QString* error)
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
        return createLocal(config, runtime);
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
        config.label = runtime.title;
        if (!config.isValid()) {
            if (error)
                *error = QStringLiteral("invalid serial runtime config");
            return nullptr;
        }
        return createSerial(config, runtime);
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
        return createSsh(config, runtime);
    }
    case TransportKind::Telnet:
        if (error)
            *error = QStringLiteral("Telnet transport is not implemented");
        return nullptr;
    case TransportKind::Custom:
        if (error)
            *error = QStringLiteral("custom transport requires an external factory");
        return nullptr;
    }
    return nullptr;
}

std::unique_ptr<TerminalSession>
SessionFactory::createLocal(const LocalShellConfig& config, RuntimeConfig runtime)
{
    runtime.transportKind = TransportKind::LocalShell;
    auto session = std::make_unique<TerminalSession>(std::move(runtime));
    auto* transport = new LocalShellTransport;
    transport->setSessionConfig(config);
    session->attach(transport, TerminalSession::Ownership::Adopt);
    return session;
}

std::unique_ptr<TerminalSession>
SessionFactory::createSerial(const SerialConfig& config, RuntimeConfig runtime)
{
    return makeSession<SerialTransport>(config, std::move(runtime), TransportKind::Serial);
}

std::unique_ptr<TerminalSession>
SessionFactory::createSsh(const SshConfig& config, RuntimeConfig runtime)
{
    return makeSession<SshTransport>(config, std::move(runtime), TransportKind::Ssh);
}
