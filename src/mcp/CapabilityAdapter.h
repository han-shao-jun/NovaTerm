/**
 * @file CapabilityAdapter.h
 * @brief 运行时把 Session 事实转换成纯应用层 `CapabilityFacts`。
 *
 * 这是 Session 与应用能力模型之间的唯一适配点：连接类型、Profile 与终端
 * 控制器分支只在这里出现，界面和业务查询不重复这些规则。
 */
#pragma once
#include "capabilities/terminal/ReadCapabilities.h"

#include "mcp/application/CapabilityResolver.h"
#include "mcp/contracts/ToolRequests.h"
#include "mcp/tasks/ReadUseCases.h"
#include "session/SessionDirectory.h"

#include <QJsonObject>

namespace NovaTerm::Mcp {

class AccessStore;
class ObservationService;

/** @brief 同时复核目录状态、实际传输与连接代际，排除已断开但目录尚未更新的会话。 */
[[nodiscard]] bool connectedSession(const SessionDirectory::Entry& entry);

[[nodiscard]] CapabilityFacts capabilityFacts(const SessionDirectory::Entry& entry,
                                              const AccessStore& access);

/** @brief 解析当前客户端在该会话上的统一能力视图。 */
[[nodiscard]] CapabilitySnapshot sessionCapabilities(const SessionDirectory::Entry& entry,
    const QString& clientId, const AccessStore& access, bool humanConfirmation);

/** @brief Session、观察服务与 MCP 令牌/结果编码的只读集成适配器。 */
class ReadUseCaseAdapter final : public ReadSessionPort
{
public:
    struct Ports
    {
        AccessStore* access;
        SessionDirectory* sessions;
        ObservationService* observation;
        QString instance;
    };

    ReadUseCaseAdapter(Ports ports, Capabilities::CapabilityDispatcher&, const Capabilities::InvocationContext&);
    [[nodiscard]] Result<QJsonObject> listSessions(const QString& client,
        const QByteArray& key, bool formElicitation, const ListSessionsRequest& request) const;
    [[nodiscard]] Result<QJsonObject> readContext(const SessionDirectory::Entry& entry,
        const QString& owner, const QString& client, const QByteArray& key,
        const ReadContextRequest& request) const;

private:
    [[nodiscard]] quint64 directoryRevision() const override;
    [[nodiscard]] ClientAccessVersion accessVersion(const ClientId& client) const override;
    [[nodiscard]] QList<ReadSession> pageAfter(const ClientId& client, const SessionId& after,
        int limit, bool humanConfirmation) const override;
    [[nodiscard]] Result<ContextSummary> readContext(const ContextQuery& query) const override;
    Ports _ports;
    Capabilities::CapabilityDispatcher& _dispatcher;
    const Capabilities::InvocationContext& _context;
};

} // namespace NovaTerm::Mcp
