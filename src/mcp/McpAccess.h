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
    /** @brief 上一次 save() 失败的成因。调用方取用后应调用 clearSaveFailure()。 */
    enum class SaveFailure {
        None,        ///< 无失败，或上次失败已被取用
        Directory,   ///< 状态目录未通过 0700 与属主校验
        Locked,      ///< 共享文件被另一个 NovaTerm 实例持锁占用
        Revision,    ///< 文件已被并发改写，需重载后重试
        Write,       ///< 写盘本身失败
    };
    struct Client { QString id; QString label; QByteArray digest; bool persistent{false}; quint64 version{1}; };
    struct Grant {
        QString attachment;
        QString epoch;
        QSet<QString> commands;
        bool interactiveCommand{false};
        bool confirmedCommand{false};
        bool scriptTask{false};
    };
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
                  bool read, QSet<QString> commands = {},
                  bool interactiveCommand = false,
                  bool confirmedCommand = false,
                  bool scriptTask = false);
    [[nodiscard]] bool canRead(const QString& clientId, const SessionDirectory::Entry& entry) const;
    [[nodiscard]] QSet<QString> commands(const QString& clientId, const SessionDirectory::Entry& entry) const;
    /** @brief 当前客户端是否单独获准在该 Session 执行交互命令。 */
    [[nodiscard]] bool canRunCommand(const QString& clientId,
                                     const SessionDirectory::Entry& entry) const;
    [[nodiscard]] bool canRunConfirmedCommand(
        const QString& clientId, const SessionDirectory::Entry& entry) const;
    [[nodiscard]] bool canRunScriptTask(
        const QString& clientId, const SessionDirectory::Entry& entry) const;
    [[nodiscard]] SaveFailure lastSaveFailure() const { return _lastSaveFailure; }
    void clearSaveFailure() { _lastSaveFailure = SaveFailure::None; }
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
    // 最近一次 save() 的成因；成功时复位为 None。
    SaveFailure _lastSaveFailure{SaveFailure::None};
    QFileSystemWatcher _watcher;
};
}
