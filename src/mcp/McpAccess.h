/** @file McpAccess.h
 *  @brief 本机 MCP 接入凭据与运行期会话授权；读取共享不隐含命令权限。
 */
#pragma once
#include "credential/CredentialStore.h"
#include "mcp/application/AuthorizationService.h"
#include "mcp/security/ClientRegistry.h"
#include "mcp/contracts/Permissions.h"
#include "session/SessionDirectory.h"
#include <QJsonObject>
#include <QSet>
#include <QFileSystemWatcher>
#include <QThread>
#include <QPointer>
#include <functional>

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
    /** @brief 兼容旧调用点的名称；实现已迁到 `ClientRegistry`。 */
    using Client = ClientRecord;
    explicit AccessStore(QString directory, std::unique_ptr<CredentialStore> credentials,
                         QObject* parent = nullptr,
                         std::function<std::unique_ptr<CredentialStore>()> elevationFactory = {});
    ~AccessStore() override;
    [[nodiscard]] bool enabled() const { return _enabled; }
    bool setEnabled(bool enabled);
    [[nodiscard]] QList<Client> clients() const;
    [[nodiscard]] QString addClient(const QString& label);
    bool removeClient(const QString& id);
    bool rotateToken(const QString& id);
    [[nodiscard]] std::optional<QByteArray> exportToken(const QString& id) const;
    [[nodiscard]] QString authenticate(const QByteArray& token) const;
    [[nodiscard]] quint64 version(const QString& clientId) const;
    /**
     * @brief 唯一授权修改入口：带连接绑定与预期行版本的单项选择。
     *
     * 拒绝结果携带当前视图；调用方不得把旧整组状态写回新连接。
     */
    [[nodiscard]] Result<GrantSnapshot, AuthorizationError> changePermission(
        const PermissionChange& change, const SessionDirectory::Entry& entry);
    /** @brief 当前绑定下的授权视图；执行类权限在重连后隐藏。 */
    [[nodiscard]] GrantSnapshot grantSnapshot(
        const ClientId& client, const SessionDirectory::Entry& entry) const;
    [[nodiscard]] bool canRead(const QString& clientId, const SessionDirectory::Entry& entry) const;
    [[nodiscard]] QSet<QString> commands(const QString& clientId, const SessionDirectory::Entry& entry) const;
    /** @brief 当前客户端是否单独获准在该 Session 执行交互命令。 */
    [[nodiscard]] bool canRunCommand(const QString& clientId,
                                     const SessionDirectory::Entry& entry) const;
    [[nodiscard]] bool canRunScriptTask(
        const QString& clientId, const SessionDirectory::Entry& entry) const;
    [[nodiscard]] bool canControlTerminal(const QString& clientId,
                                          const SessionDirectory::Entry& entry) const;
    /** @brief 独立清空授权；只对当前就绪连接有效，不由完整输入授权推导。 */
    [[nodiscard]] bool canClearTerminal(const QString& clientId,
                                       const SessionDirectory::Entry& entry) const;
    [[nodiscard]] bool canUseElevation(const QString& clientId,
                                       const SessionDirectory::Entry& entry) const;
    /** @brief 新授予提权权限的阻止原因；不以目标/适配器的暂不可用限制授权选择。 */
    [[nodiscard]] QString elevationGrantDisabledReason(
        const QString& clientId, const SessionDirectory::Entry& entry) const;
    /** @brief 实际执行提权所缺少的条件，与授权选择分离；不访问密码。 */
    [[nodiscard]] QString elevationExecutionUnavailableReason(const SessionDirectory::Entry& entry) const;
    /** @brief 目标身份是否已通过 Profile/指纹核验；不读取凭据。 */
    [[nodiscard]] bool elevationTargetIdentityVerified(const SessionDirectory::Entry& entry) const;
    /** @brief 凭据工作线程是否正在变更（新提权授权需等待）。 */
    [[nodiscard]] bool credentialUpdatePending() const { return _credentialOperationPending; }
    // 本机配置入口。协议处理和状态读取均不取密；没有公开的取密接口。
    bool setElevationCredential(const SessionDirectory::Entry& entry,
                                const QString& method, const QByteArray& secret);
    bool removeElevationCredential(const SessionDirectory::Entry& entry, const QString& method);
    [[nodiscard]] bool hasElevationCredential(const SessionDirectory::Entry& entry,
                                              const QString& method) const;
    [[nodiscard]] bool elevationCredentialsPersistent() const;
    /** @brief 本机 UI 的有界异步存储入口；工作线程拥有独立的系统凭据连接。 */
    bool configureElevationCredentialAsync(const SessionDirectory::Entry& entry,
        const QString& method, QByteArray secret, bool remove,
        std::function<void(bool)> finished);
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
    [[nodiscard]] QString elevationReference(const SessionDirectory::Entry& entry,
                                              const QString& method) const;
    void revokeElevation(const QString& targetFingerprint);
    [[nodiscard]] std::optional<GrantRow> grantRow(
        const QString& clientId, const SessionDirectory::Entry& entry) const;
    [[nodiscard]] static GrantFacts factsOf(const SessionDirectory::Entry& entry);
    QString _directory;
    mutable std::unique_ptr<CredentialStore> _credentials;
    ClientRegistry _clients;
    AuthorizationService _authorization;
    QSet<QString> _elevationReferences; // 非秘密元数据；状态展示不访问系统凭据库。
    std::function<std::unique_ptr<CredentialStore>()> _elevationFactory;
    struct CredentialMailbox;
    std::shared_ptr<CredentialMailbox> _elevationMailbox;
    QPointer<QThread> _elevationThread;
    QObject* _elevationWorker{nullptr};
    std::shared_ptr<std::unique_ptr<CredentialStore>> _elevationStorage;
    std::optional<bool> _elevationPersistent;
    bool _credentialOperationPending{false};
    bool _enabled{false};
    quint64 _storeRevision{0};
    quint64 _elevationRevision{0};
    // 最近一次 save() 的成因；成功时复位为 None。
    SaveFailure _lastSaveFailure{SaveFailure::None};
    QFileSystemWatcher _watcher;
};
}
