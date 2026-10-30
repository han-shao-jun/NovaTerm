#pragma once
#include <QElapsedTimer>
#include <QJsonObject>
#include <QMap>
#include <optional>
#include <functional>

namespace NovaTerm::Mcp {
// Application-thread confirmation state. No file contents or transport I/O.
class UploadConfirmations final {
public:
    using Now = std::function<qint64()>;
    explicit UploadConfirmations(QString stateDirectory, Now now = {});
    struct Intent {
        QString client, identity, connection, binding, targetKey, target;
        QString sessionId, epoch, operationId, localPath, remotePath, protocol;
        bool overwrite{false};
        QString memoryIdentity{};
    };
    struct Decision { bool allowed{false}; QJsonObject reply; };
    Decision check(const Intent&, const QString& token = {}, std::optional<bool> confirmed = {});
    bool clear();
    void disconnect(const QString& connection);
    void invalidate();
private:
    struct Pending {
        Intent intent;
        QString signature, memoryKey, token, generation;
        qint64 expires{0};
        std::optional<bool> answer{};
    };
    QString _file;
    QElapsedTimer _clock;
    Now _now;
    QMap<QString, Pending> _pending;
};
}
