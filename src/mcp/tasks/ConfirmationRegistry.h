/**
 * @file ConfirmationRegistry.h
 * @brief 两代协议共用的确认绑定校验、一次消费与待确认记录。
 *
 * 这里只保存值（绑定、摘要、代际、过期与作用域），不持 Job/Session 指针；
 * 任务完成、撤权、过期与断连都由调用方按记录中的键解析后处理。
 */
#pragma once

#include "mcp/contracts/ToolRequests.h"

#include <QByteArray>
#include <QHash>
#include <QString>

#include <cstddef>
#include <optional>
#include <vector>

namespace NovaTerm::Mcp {

/** @brief 待确认的人类确认记录；tool 决定摘要/风险复核方式。 */
struct PendingConfirmationRecord
{
    QString jobKey; ///< 作业索引键；调用方据此解析仍存活的 Job。
    QString command;
    ConfirmationTransaction transaction;
};

class ConfirmationRegistry
{
public:
    /** @brief 两代协议共用的绑定与到期验证；当前事实由执行用例取得。 */
    [[nodiscard]] static bool validate(const ConfirmationTransaction& transaction,
        const ConfirmationBinding& current, qint64 now);
    /** @brief MRTR 验证后一次消费；失败不消费，不延长期限。 */
    [[nodiscard]] bool consume(const ConfirmationTransaction& transaction,
        const ConfirmationBinding& current, qint64 now);
    void registerActive(const ConfirmationTransaction& transaction);
    void registerActive(const QString& connection, const QString& client,
        const QString& session, const QString& nonce, qint64 expiry);
    void clearActive(const QString& connection, const QString& nonce);
    void consumeActive(const QString& connection, const QString& nonce, qint64 expiry);
    [[nodiscard]] bool isActive(const QString& connection, const QString& nonce) const;
    [[nodiscard]] bool hasActiveForConnection(const QString& connection) const;
    [[nodiscard]] bool hasActiveForSession(const SessionId& session, qint64 now) const;
    [[nodiscard]] bool isConsumed(const QString& nonce) const;
    /** @brief 断开连接：返回并清理该连接的活动 nonce。 */
    [[nodiscard]] QString takeConnection(const QString& connection);
    /** @brief 撤权：移除匹配客户端/会话的 nonce 作用域。 */
    void removeScope(const QString& client, const QString& session);

    void insertPending(const QString& key, PendingConfirmationRecord record);
    [[nodiscard]] std::optional<PendingConfirmationRecord> takePending(const QString& key);
    bool removePending(const QString& key);
    [[nodiscard]] bool hasPendingForConnection(const QString& connection) const;
    [[nodiscard]] std::size_t pendingCount() const { return std::size_t(_pending.size()); }

    /**
     * @brief 清理到期 nonce，并移除/返回到期待确认记录。
     *
     * 调用方对返回的记录解析 Job 并发 `COMMAND_CONFIRMATION_EXPIRED`。
     */
    [[nodiscard]] std::vector<PendingConfirmationRecord> sweep(qint64 now);

    [[nodiscard]] bool empty() const;
    void clear();

private:
    struct Scope
    {
        QString connection;
        QString client;
        QString session;
        qint64 expires{0};
    };

    QHash<QString, QString> _active;      ///< connection → nonce
    QHash<QString, Scope> _scopes;        ///< nonce → scope/expiry
    QHash<QString, qint64> _consumed;     ///< nonce → expiry
    QHash<QString, PendingConfirmationRecord> _pending;
};

} // namespace NovaTerm::Mcp
