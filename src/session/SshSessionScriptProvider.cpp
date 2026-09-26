/** @file SshSessionScriptProvider.cpp
 *  @brief 用与活动 SSH Session 相同的凭据和 known_hosts 写入脚本字节。
 */
#include "SshSessionScriptProvider.h"

#include "transport/SshTransport.h"

#include <QCryptographicHash>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonDocument>

SshSessionScriptProvider::SshSessionScriptProvider(SshTransport* transport,
                                                   QObject* parent)
    : ISessionScriptProvider(parent)
    , _transport(transport)
    , _sftp(nullptr)
{
    connect(&_sftp, &SftpSession::connected, this,
        [this](const QString&) {
            if (!_activeRequest)
                return;
            if (!activeIdentityMatches()) {
                finishWrite(false, QStringLiteral("SCRIPT_TARGET_SESSION_CHANGED"));
                return;
            }
            _sftpConnectionGeneration = _expectedConnectionGeneration;
            if (_cancelled) {
                finishWrite(false, QStringLiteral("SCRIPT_UPLOAD_CANCELLED"));
                return;
            }
            beginUpload();
        });
    connect(&_sftp, &SftpSession::uploadBytesFinished, this,
        [this](quint64 requestId, const QString&, bool success,
               const QString& errorCode) {
            if (!_activeRequest || _activeRequest->requestId != requestId)
                return;
            if (!activeIdentityMatches()) {
                finishWrite(false, QStringLiteral("SCRIPT_TARGET_SESSION_CHANGED"));
                return;
            }
            finishWrite(success, errorCode);
        });
    connect(&_sftp, &SftpSession::errorOccurred, this,
        [this](const QString&) {
            if (!_activeRequest)
                return;
            finishWrite(false, _cancelled
                ? QStringLiteral("SCRIPT_UPLOAD_CANCELLED")
                : QStringLiteral("SCRIPT_WRITE_FAILED"));
        });
    connect(&_sftp, &SftpSession::disconnected, this,
        [this] {
            if (_activeRequest && !_uploadStarted)
                finishWrite(false, _cancelled
                    ? QStringLiteral("SCRIPT_UPLOAD_CANCELLED")
                    : QStringLiteral("SCRIPT_SFTP_UNAVAILABLE"));
        });
}

SshSessionScriptProvider::~SshSessionScriptProvider()
{
    if (_activeRequest)
        cancelWrite(_activeRequest->requestId);
    _sftp.disconnectFromHost();
}

bool SshSessionScriptProvider::isAvailable() const
{
    return _transport && _transport->isConnected()
        && _transport->sessionConfig().isValid()
        && !_transport->serverHostKeyFingerprint().isEmpty();
}

QString SshSessionScriptProvider::resolveRemotePath(
    const ScriptWriteRequest& request)
{
    const QString target = request.targetPath.trimmed();
    const QString directory = request.workingDirectory.trimmed();
    if (target.isEmpty() || directory.isEmpty()
        || target.contains(QChar::Null) || directory.contains(QChar::Null)) {
        return {};
    }
    if (target.startsWith(QLatin1Char('/')))
        return QDir::cleanPath(target);
    return QDir::cleanPath(directory + QLatin1Char('/') + target);
}

QByteArray SshSessionScriptProvider::identityDigest(const SshConfig& config)
{
    const QJsonObject identity{{"host", config.host.trimmed().toLower()},
        {"port", int(config.port)}, {"username", config.username.trimmed()},
        {"authMethod", config.authMethod}, {"privateKeyPath", config.privateKeyPath},
        {"password", config.password}, {"keyPassphrase", config.keyPassphrase}};
    return QCryptographicHash::hash(
        QJsonDocument(identity).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256);
}

bool SshSessionScriptProvider::activeIdentityMatches() const
{
    return _transport && _transport->isConnected()
        && _transport->connectionGeneration() == _expectedConnectionGeneration
        && _transport->serverHostKeyFingerprint() == _expectedHostKeyFingerprint
        && _transport->knownHostsPath() == _expectedKnownHostsPath
        && identityDigest(_transport->sessionConfig()) == _identityDigest;
}

bool SshSessionScriptProvider::writeScript(
    const ScriptWriteRequest& request)
{
    if (_activeRequest || request.requestId == 0 || request.content.isEmpty()
        || request.content.size() > MaxScriptBytes || !isAvailable()) {
        return false;
    }
    _resolvedPath = resolveRemotePath(request);
    if (_resolvedPath.isEmpty())
        return false;

    _activeRequest = request;
    _activeRequest->targetPath = _resolvedPath;
    _identityDigest = identityDigest(_transport->sessionConfig());
    _expectedHostKeyFingerprint = _transport->serverHostKeyFingerprint();
    _expectedKnownHostsPath = _transport->knownHostsPath();
    _expectedConnectionGeneration = _transport->connectionGeneration();
    _uploadStarted = false;
    _cancelled = false;

    if (_sftp.isConnected()
        && _sftpConnectionGeneration == _expectedConnectionGeneration) {
        beginUpload();
    } else {
        _sftp.disconnectFromHost();
        if (!activeIdentityMatches()) {
            finishWrite(false, QStringLiteral("SCRIPT_TARGET_SESSION_CHANGED"));
            return true;
        }
        _sftp.connectToHost(_transport->sessionConfig(),
                            _expectedHostKeyFingerprint,
                            _expectedKnownHostsPath);
    }
    return true;
}

void SshSessionScriptProvider::beginUpload()
{
    if (!_activeRequest || _uploadStarted)
        return;
    if (_cancelled) {
        finishWrite(false, QStringLiteral("SCRIPT_UPLOAD_CANCELLED"));
        return;
    }
    if (!activeIdentityMatches()) {
        finishWrite(false, QStringLiteral("SCRIPT_TARGET_SESSION_CHANGED"));
        return;
    }
    _uploadStarted = _sftp.uploadBytes(_activeRequest->requestId,
        _activeRequest->content, _resolvedPath);
    if (!_uploadStarted)
        finishWrite(false, QStringLiteral("SCRIPT_WRITE_BUSY"));
}

void SshSessionScriptProvider::cancelWrite(quint64 requestId)
{
    if (!_activeRequest || _activeRequest->requestId != requestId)
        return;
    _cancelled = true;
    if (_uploadStarted)
        _sftp.cancelUpload(requestId);
}

void SshSessionScriptProvider::finishWrite(bool success, QString errorCode)
{
    if (!_activeRequest)
        return;
    ScriptWriteResult result;
    result.requestId = _activeRequest->requestId;
    result.success = success;
    result.errorCode = std::move(errorCode);
    result.resolvedTargetPath = _resolvedPath;
    result.contentHash = QCryptographicHash::hash(
        _activeRequest->content, QCryptographicHash::Sha256);
    _activeRequest.reset();
    _identityDigest.clear();
    _expectedHostKeyFingerprint.clear();
    _expectedKnownHostsPath.clear();
    _expectedConnectionGeneration = 0;
    _uploadStarted = false;
    _cancelled = false;
    emit finished(result);
}
