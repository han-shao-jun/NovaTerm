/** @file McpService.h
 *  @brief 应用级 MCP 门面：目录、授权、请求调度和受限命令。
 */
#pragma once
#include "McpAccess.h"
#include "application/TargetConnections.h"
#include <QJsonArray>
#include <memory>
namespace NovaTerm::Recording { class HistoryService; }
namespace NovaTerm::Resources { class ResourceSharingService; }

namespace NovaTerm::Mcp {
class HistoryCapabilityAdapter;
class SavedTargets;
class Service final : public QObject
{
    Q_OBJECT
public:
    explicit Service(QString stateDirectory = {}, QString instanceDirectory = {},
                     std::unique_ptr<CredentialStore> credentials = {}, QObject* parent = nullptr, quint16 port = 0,
                     std::shared_ptr<Resources::ResourceSharingService> resources = {});
    ~Service() override;
    [[nodiscard]] AccessStore& access();
    [[nodiscard]] SessionDirectory& directory();
    [[nodiscard]] SavedTargets& savedTargets();
    void setTargetOpener(TargetConnections::Open);
    [[nodiscard]] HistoryCapabilityAdapter& history();
    [[nodiscard]] std::shared_ptr<Resources::ResourceSharingService> resources() const;
    void setHistorySource(Recording::HistoryService*);
    [[nodiscard]] QString instanceId() const;
    [[nodiscard]] QString endpoint() const;
    [[nodiscard]] QString httpEndpoint() const;
    [[nodiscard]] QString status() const;
    [[nodiscard]] QJsonObject clientConfiguration(const QString& clientId) const;
    [[nodiscard]] QJsonObject ccSwitchConfiguration(const QString& clientId) const;
    [[nodiscard]] QJsonArray executionRecords() const;
    /** @brief 不含正文、搜索词、目标和凭据的本机诊断计数。 */
    [[nodiscard]] QJsonObject statistics() const;
    [[nodiscard]] QJsonObject protectedTargets() const;
    bool acknowledgeTarget(const QString& fingerprint);
    bool acknowledgeTarget(const QString& fingerprint, const QString& expectedExecutionId);
    void stop();
signals:
    void changed();
private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
}
