/**
 * @file SshSessionCommandExecutor.cpp
 * @brief SSH bounded exec 到通用 Session 命令契约的适配。
 */
#include "SshSessionCommandExecutor.h"

#include "transport/SshTransport.h"

#include <QCryptographicHash>

SshSessionCommandExecutor::SshSessionCommandExecutor(SshTransport* transport,
                                                     QObject* parent)
    : ISessionCommandExecutor(parent)
    , _transport(transport)
{
    if (_transport) {
        connect(_transport, &SshTransport::boundedCommandFinished, this,
                &ISessionCommandExecutor::finished);
    }
}

bool SshSessionCommandExecutor::isAvailable() const
{
    return _transport && _transport->isConnected();
}

CommandExecutorCapabilities SshSessionCommandExecutor::capabilities() const
{
    return {CommandExecutionMode::Isolated, true, true, true};
}

CommandPlatformProfile SshSessionCommandExecutor::profile() const
{
    return CommandPlatformProfile::sshLinux();
}

QString SshSessionCommandExecutor::targetFingerprint() const
{
    if (!isAvailable())
        return {};
    const QString hostKey = _transport->serverHostKeyFingerprint();
    const auto& config = _transport->sessionConfig();
    if (hostKey.isEmpty() || config.username.contains(QChar::Null))
        return {};
    const QByteArray identity = hostKey.toUtf8() + '\0'
        + QByteArray::number(config.port) + '\0'
        + config.username.trimmed().toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

bool SshSessionCommandExecutor::execute(const CommandExecutionRequest& request)
{
    if (!isAvailable() || request.requestId == 0 || request.command.isEmpty())
        return false;
    return _transport->executeBoundedCommand(request.requestId, request.command,
                                             request.limits);
}

void SshSessionCommandExecutor::cancel(quint64 requestId)
{
    if (_transport)
        _transport->cancelCommand(requestId);
}
