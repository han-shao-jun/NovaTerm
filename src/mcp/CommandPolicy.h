/** @file CommandPolicy.h
 *  @brief 固定诊断允许列表和跨进程目标保护标记；没有自由 shell 入口。
 */
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <optional>

class CommandPlatformProfile;

namespace NovaTerm::Mcp {
struct CommandTemplate { QString id; QString title; QByteArray command; };
class CommandPolicy final
{
public:
    [[nodiscard]] static QList<CommandTemplate> catalog();
    [[nodiscard]] static QList<CommandTemplate> catalog(
        const CommandPlatformProfile& profile);
    [[nodiscard]] static std::optional<CommandTemplate> find(const QString& id);
    [[nodiscard]] static std::optional<CommandTemplate> find(
        const QString& id, const CommandPlatformProfile& profile);
    [[nodiscard]] static QString version();
    [[nodiscard]] static QString version(const CommandPlatformProfile& profile);
};

/** @brief 持久标记先于 exec 提交；拿不到锁或标记损坏时失败关闭。 */
class TargetGuard final
{
public:
    explicit TargetGuard(QString directory);
    [[nodiscard]] bool reserve(const QString& fingerprint, const QString& executionId,
                               const QString& instanceId, QString& error);
    bool release(const QString& fingerprint, const QString& executionId);
    [[nodiscard]] QJsonObject markers() const;
    /** @note 只由用户界面在人工核对后调用，不暴露给 MCP。 */
    bool acknowledge(const QString& fingerprint);
private:
    QString _directory;
};
}
