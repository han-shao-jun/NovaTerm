#pragma once
#include "FileResourceAdapter.h"
#include "contracts/FileTransferRequest.h"
#include "contracts/FileTransferPaths.h"
#include "session/SftpFileUpload.h"
#include <QElapsedTimer>
#include <QJsonObject>
#include <QTimer>
#include <memory>

namespace NovaTerm::Mcp {
// GUI-thread bounded index. SftpSession remains the only transfer execution owner.
class FileTransfers final : public QObject {
public:
    struct Delivery {
        std::function<void(quint64, quint64)> progress;
        std::function<void()> publicationReady;
        std::function<void(const SftpFileUploadResult&)> finished;
    };
    using Start = std::function<bool(TerminalSession&, const SftpFileUploadRequest&, Delivery)>;
    FileTransfers(AccessStore&, SessionDirectory&, FileResourceAdapter&, QString instance,
        QObject* parent = nullptr, Start = {});
    ~FileTransfers() override;
    FileTransfers(const FileTransfers&) = delete;
    FileTransfers& operator=(const FileTransfers&) = delete;
    Result<QJsonObject> call(const QString& client, quint64 identity, const QString& connection,
        const FileTransferRequest&);
    // Pure retained-operation lookup; confirmation metadata does not change transfer identity.
    std::optional<Result<QJsonObject>> replay(const QString& client, quint64 identity,
        const FileTransferRequest&) const;
    bool busy(const QString& session) const;
    void disconnect(const QString& connection);
    void stop();
    void setStarter(Start start) { _start = std::move(start); }
private:
    struct Record;
    struct Authority { TransferPath local, remote; SessionBinding binding; QPointer<TerminalSession> session; };
    Result<Authority> authorize(const QString&, quint64, const StartFileUploadRequest&, SftpFileTransferDirection);
    bool valid(const std::shared_ptr<Record>&);
    void cancel(const std::shared_ptr<Record>&);
    void check();
    QJsonObject project(const Record&) const;
    AccessStore& _access;
    SessionDirectory& _sessions;
    FileResourceAdapter& _resources;
    QString _instance;
    Start _start;
    QMap<QString, std::shared_ptr<Record>> _records;
    QElapsedTimer _clock;
    QTimer _timer;
    quint64 _nextRequest{0};
    bool _checking{false};
};
}
