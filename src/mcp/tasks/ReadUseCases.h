/**
 * @file ReadUseCases.h
 * @brief 只读工具用例（目录、上下文读取、诊断目录）的适配层。
 *
 * 只依赖应用服务与会话目录，不持有任务/确认/执行集合；JSON 结果在用例
 * 边界组装，Service 只负责完成作业。
 */
#pragma once

#include "mcp/contracts/Errors.h"
#include "mcp/contracts/ToolRequests.h"
#include "session/SessionDirectory.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace NovaTerm::Mcp {

class AccessStore;
class ObservationService;
class TerminalInteraction;

class ReadUseCases
{
public:
    struct Ports
    {
        AccessStore* access{nullptr};
        SessionDirectory* sessions{nullptr};
        ObservationService* observation{nullptr};
        TerminalInteraction* terminal{nullptr};
        QString instance;
    };

    explicit ReadUseCases(Ports ports);

    [[nodiscard]] Result<QJsonObject> listSessions(const QString& client,
        const QByteArray& key, bool formElicitation,
        const ListSessionsRequest& request) const;
    [[nodiscard]] Result<QJsonObject> readContext(const SessionDirectory::Entry& entry,
        const QString& owner, const QString& client, const QByteArray& key,
        const ReadContextRequest& request) const;
    [[nodiscard]] Result<QJsonObject> listCommands(const SessionDirectory::Entry& entry,
        const QString& client, const QByteArray& key, qint64 now) const;

private:
    Ports _ports;
};

} // namespace NovaTerm::Mcp
