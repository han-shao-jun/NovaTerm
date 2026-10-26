/**
 * @file CaptureStore.h
 * @brief 兼容连接作用域与客户端作用域的统一 capture 存储。
 *
 * 两种作用域分别计额，共享物理存储但不把两份有效容量压成较小上限：
 * 每连接 4、每客户端 4、客户端作用域全局 32（总上界由两部分相加）。
 * 只保存有界投影，不保存终端历史；淘汰使用单调序号保证 FIFO。
 */
#pragma once

#include <QHash>
#include <QString>
#include <QVector>
#include <QString>

#include <cstddef>
#include <functional>
#include <optional>

namespace NovaTerm::Mcp {

/** @brief capture 的有效期作用域；不是连接类型。 */
enum class CaptureScope
{
    Connection, ///< 兼容旧 read_context：绑定连接与 epoch。
    Client,     ///< 客户端观察 token：绑定客户端、grant 与 epoch。
};

/** @brief 有界投影中的一行；source 区分视口与最近输出。 */
struct CaptureLine
{
    QString text;
    QString source;
    int row{0};
};

struct CaptureRecord
{
    CaptureScope scope{CaptureScope::Connection};
    QString owner; ///< Connection：连接 ID；Client：客户端 ID。
    QString session;
    QString epoch;
    quint64 grantVersion{0};
    QString revision;
    QVector<CaptureLine> lines; ///< 有界投影，每行带 source 标签。
    bool sourceTruncated{false};
    bool outputTruncated{false};
    qint64 expires{0};
    quint64 sequence{0}; ///< 插入序号，由存储分配。
    qint64 created{0};
    quint64 cacheGeneration{0};
};

struct CaptureQuotas
{
    int perConnection{4};
    int perClient{4};
    int clientScopeTotal{32};
};

class CaptureStore
{
public:
    explicit CaptureStore(CaptureQuotas quotas = {});

    void put(QString id, CaptureRecord record);
    [[nodiscard]] std::optional<CaptureRecord> find(const QString& id) const;
    [[nodiscard]] bool contains(const QString& id) const;
    /** @brief 客户端作用域且属于该客户端；连接作用域返回 false。 */
    [[nodiscard]] bool ownedByClient(const QString& id, const QString& client) const;

    bool remove(const QString& id);
    void removeSession(const QString& session);
    void removeConnection(const QString& connection);
    void removeConnectionSession(const QString& connection, const QString& session);
    void removeClient(const QString& client);
    void removeClientSession(const QString& client, const QString& session);
    void sweep(qint64 now);
    void clear();
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t size(CaptureScope scope, const QString& owner) const;
    [[nodiscard]] std::size_t size(CaptureScope scope) const;

private:
    [[nodiscard]] std::size_t countIf(
        const std::function<bool(const CaptureRecord&)>& predicate) const;
    void eraseOldest(CaptureScope scope, const QString& owner, bool anyOwner);

    QHash<QString, CaptureRecord> _records;
    CaptureQuotas _quotas;
    quint64 _sequence{0};
};

} // namespace NovaTerm::Mcp
