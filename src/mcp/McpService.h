/** @file McpService.h
 *  @brief 应用级 MCP 门面：目录、授权、请求调度和受限命令。
 */
#pragma once
#include "McpAccess.h"
#include <QJsonArray>
#include <memory>

namespace NovaTerm::Mcp {
class Service final : public QObject
{
    Q_OBJECT
public:
    explicit Service(QString stateDirectory = {}, QString instanceDirectory = {},
                     std::unique_ptr<CredentialStore> credentials = {}, QObject* parent = nullptr);
    ~Service() override;
    [[nodiscard]] AccessStore& access();
    [[nodiscard]] SessionDirectory& directory();
    [[nodiscard]] QString instanceId() const;
    [[nodiscard]] QString endpoint() const;
    [[nodiscard]] QString status() const;
    [[nodiscard]] QJsonObject clientConfiguration(const QString& clientId) const;
    [[nodiscard]] QJsonArray executionRecords() const;
    /** @brief 不含正文、搜索词、目标和凭据的本机诊断计数。 */
    [[nodiscard]] QJsonObject statistics() const;
    [[nodiscard]] QJsonObject protectedTargets() const;
    bool acknowledgeTarget(const QString& fingerprint);
    void stop();
signals:
    void changed();
private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
}
