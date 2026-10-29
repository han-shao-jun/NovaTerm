/** @file McpService.h
 *  @brief 应用级 MCP 门面：目录、授权、请求调度和受限命令。
 */
#pragma once
#include "McpAccess.h"
#include "FileTransfers.h"
#include <QJsonArray>
#include <memory>
namespace NovaTerm::Recording { class HistoryService; }
namespace NovaTerm::Resources { class ResourceSharingService; }

namespace NovaTerm::Mcp {
class HistoryCapabilityAdapter;
class FileResourceAdapter;
class SerialFileTargetAdapter;
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
    [[nodiscard]] FileResourceAdapter& fileResources();
    [[nodiscard]] SerialFileTargetAdapter& serialFileTargets();
    void setFileUploadStarter(FileTransfers::Start);
    [[nodiscard]] HistoryCapabilityAdapter& history();
    [[nodiscard]] std::shared_ptr<Resources::ResourceSharingService> resources() const;
    void setHistorySource(Recording::HistoryService*);
    [[nodiscard]] QString instanceId() const;
    [[nodiscard]] QString endpoint() const;
    [[nodiscard]] QString httpEndpoint() const;
    [[nodiscard]] QString status() const;
    [[nodiscard]] QJsonObject clientConfiguration() const;
    [[nodiscard]] QJsonObject ccSwitchConfiguration() const;
    /** @brief 不含正文、搜索词、目标和凭据的本机诊断计数。 */
    [[nodiscard]] QJsonObject statistics() const;
    void stop();
signals:
    void changed();
private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
}
