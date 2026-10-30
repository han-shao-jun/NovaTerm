#pragma once
#include "FileResourceAdapter.h"
#include "SerialFileTargetAdapter.h"
#include "contracts/FileTransferRequest.h"
#include "contracts/FileTransferPaths.h"
#include "session/transfer/SerialFileTransferController.h"
#include <QElapsedTimer>
#include <QTimer>

struct SerialTransferResult;

namespace NovaTerm::Mcp {
// GUI-thread facts only; the Session controller owns all protocol and file work.
class SerialFileTransfers final : public QObject {
public:
    using ControllerResolver = std::function<SerialFileTransferController*(TerminalSession&)>;
    using Now = std::function<qint64()>;
    SerialFileTransfers(AccessStore&, SessionDirectory&, FileResourceAdapter&, SerialFileTargetAdapter&,
        QString instance, QObject* parent = nullptr, ControllerResolver = {}, Now = {});
    ~SerialFileTransfers() override;
    SerialFileTransfers(const SerialFileTransfers&) = delete;
    SerialFileTransfers& operator=(const SerialFileTransfers&) = delete;
    Result<QJsonObject> call(const QString& client, quint64 identity, const QString& connection,
        const SerialFileTransferRequest&);
    // Pure retained-operation lookup; confirmation metadata does not change transfer identity.
    std::optional<Result<QJsonObject>> replay(const QString& client, quint64 identity,
        const SerialFileTransferRequest&) const;
    bool busy(const QString& session) const;
    void disconnect(const QString& connection);
    void stop();
private:
    struct Record;
    struct Authority { TransferPath local; SerialFileBinding binding; QPointer<TerminalSession> session; int baudRate; };
    Result<Authority> authorize(const QString&, quint64, const StartSerialFileUploadRequest&);
    bool valid(const std::shared_ptr<Record>&);
    bool currentBinding(const std::shared_ptr<Record>&);
    bool admit(const std::shared_ptr<Record>&);
    void cancel(const std::shared_ptr<Record>&);
    void check();
    void observe(const std::shared_ptr<Record>&, const SerialTransferProgress&);
    void finish(const std::shared_ptr<Record>&, const SerialTransferResult&);
    void settle(const std::shared_ptr<Record>&, const QString& state, const QString& error);
    QJsonObject project(const Record&) const;
    qint64 now() const;
    AccessStore& _access;
    SessionDirectory& _sessions;
    SerialFileTargetAdapter& _targets;
    QString _instance;
    ControllerResolver _resolve;
    Now _now;
    QMap<QString, std::shared_ptr<Record>> _records;
    QElapsedTimer _clock;
    QTimer _timer;
    bool _checking{false};
};
}
