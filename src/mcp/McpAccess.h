/** @file McpAccess.h
 *  @brief 本机 MCP 接入凭据与运行期会话授权；读取共享不隐含命令权限。
 */
#pragma once
#include "credential/CredentialStore.h"
#include "session/SessionDirectory.h"
#include <QJsonObject>
#include <QSet>
#include <QFileSystemWatcher>

namespace NovaTerm::Mcp {
class AccessStore final : public QObject
{
    Q_OBJECT
public:
    struct Client { QString id; QString label; QByteArray digest; bool persistent{false}; quint64 version{1}; };
    struct Grant { QString attachment; QString epoch; QSet<QString> commands; };
    explicit AccessStore(QString directory, std::unique_ptr<CredentialStore> credentials,
                         QObject* parent = nullptr);
    [[nodiscard]] bool enabled() const { return _enabled; }
    bool setEnabled(bool enabled);
    [[nodiscard]] QList<Client> clients() const;
    [[nodiscard]] QString addClient(const QString& label);
    bool removeClient(const QString& id);
    bool rotateToken(const QString& id);
    [[nodiscard]] std::optional<QByteArray> exportToken(const QString& id) const;
    [[nodiscard]] QString authenticate(const QByteArray& token) const;
    [[nodiscard]] quint64 version(const QString& clientId) const;
    bool setGrant(const QString& clientId, const SessionDirectory::Entry& entry,
                  bool read, QSet<QString> commands = {});
    [[nodiscard]] bool canRead(const QString& clientId, const SessionDirectory::Entry& entry) const;
    [[nodiscard]] QSet<QString> commands(const QString& clientId, const SessionDirectory::Entry& entry) const;
signals:
    void changed();
    void revoked(const QString& clientId, const QString& sessionId);
private:
    bool save();
    void reload();
    [[nodiscard]] CredentialStore* credentials() const;
    [[nodiscard]] static QString reference(const QString& id);
    QString _directory;
    mutable std::unique_ptr<CredentialStore> _credentials;
    QHash<QString, Client> _clients;
    QHash<QString, QHash<QString, Grant>> _grants;
    bool _enabled{false};
    quint64 _storeRevision{0};
    QFileSystemWatcher _watcher;
};
}
