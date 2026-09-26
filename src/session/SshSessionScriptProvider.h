/** @file SshSessionScriptProvider.h
 *  @brief 绑定当前 SSH Transport 身份的 SFTP 脚本写入适配器。
 */
#pragma once

#include "ISessionScriptProvider.h"
#include "SftpSession.h"

#include <QPointer>
#include <optional>

class SshTransport;

class SshSessionScriptProvider final : public ISessionScriptProvider
{
    Q_OBJECT
public:
    explicit SshSessionScriptProvider(SshTransport* transport,
                                     QObject* parent = nullptr);
    ~SshSessionScriptProvider() override;

    [[nodiscard]] bool isAvailable() const override;
    bool writeScript(const ScriptWriteRequest& request) override;
    void cancelWrite(quint64 requestId) override;

private:
    [[nodiscard]] static QString resolveRemotePath(
        const ScriptWriteRequest& request);
    [[nodiscard]] static QByteArray identityDigest(const SshConfig& config);
    [[nodiscard]] bool activeIdentityMatches() const;
    void beginUpload();
    void finishWrite(bool success, QString errorCode);

    QPointer<SshTransport> _transport;
    SftpSession _sftp;
    std::optional<ScriptWriteRequest> _activeRequest;
    QString _resolvedPath;
    QByteArray _identityDigest;
    QString _expectedHostKeyFingerprint;
    QString _expectedKnownHostsPath;
    quint64 _expectedConnectionGeneration{0};
    quint64 _sftpConnectionGeneration{0};
    bool _uploadStarted{false};
    bool _cancelled{false};
    static constexpr qsizetype MaxScriptBytes = 2 * 1024 * 1024;
};
