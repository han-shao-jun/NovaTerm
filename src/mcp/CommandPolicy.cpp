/** @file CommandPolicy.cpp
 *  @brief 无参数模板与目标保护标记实现；不读取凭据或用户 shell 配置。
 */
#include "CommandPolicy.h"
#include "McpProtocol.h"
#include "session/CommandPlatformProfile.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>

namespace NovaTerm::Mcp {
QString CommandPolicy::version() { return QStringLiteral("linux-diagnostics-v2"); }
QList<CommandTemplate> CommandPolicy::catalog()
{
    // 固定诊断走当前交互 Shell 的正常命令解析；无参数和复合语法。
    return {{"system.identity", "System identity", "uname -srm"},
        {"system.uptime", "Uptime and load", "uptime"},
        {"memory.summary", "Memory summary", "free -k"},
        {"filesystem.usage", "Filesystem capacity", "df -Pk"}};
}
std::optional<CommandTemplate> CommandPolicy::find(const QString& id)
{
    for (const auto& command : catalog()) {
        if (command.id == id)
            return command;
    }
    return std::nullopt;
}

QList<CommandTemplate> CommandPolicy::catalog(
    const CommandPlatformProfile& profile)
{
    QList<CommandTemplate> result;
    if (!profile.isAvailable())
        return result;
    for (const auto& command : catalog()) {
        if (profile.supports(command.id))
            result.append(command);
    }
    return result;
}

std::optional<CommandTemplate> CommandPolicy::find(
    const QString& id, const CommandPlatformProfile& profile)
{
    if (!profile.supports(id))
        return std::nullopt;
    return find(id);
}

QString CommandPolicy::version(const CommandPlatformProfile& profile)
{
    return profile.version();
}

TargetGuard::TargetGuard(QString directory) : _directory(std::move(directory)) {}
bool TargetGuard::reserve(const QString& fingerprint, const QString& executionId,
                          const QString& instanceId, QString& error)
{
    if (fingerprint.size() != 64 || !secureDirectory(_directory)) {
        error = "COMMAND_UNAVAILABLE";
        return false;
    }
    QLockFile lock(QDir(_directory).filePath("targets.lock"));
    if (!lock.tryLock(0)) { error = "BUSY"; return false; }
    const auto path = QDir(_directory).filePath("targets.json");
    const auto saved = readJson(path, 128 * 1024);
    if (!saved && QFileInfo::exists(path)) { error = "COMMAND_EXECUTION_QUARANTINED"; return false; }
    auto markers = saved.value_or(QJsonObject{});
    if (markers.contains(fingerprint)) { error = "COMMAND_EXECUTION_QUARANTINED"; return false; }
    if (markers.size() >= 128) { error = "BUSY"; return false; }
    markers.insert(fingerprint, QJsonObject{{"executionId", executionId}, {"instanceId", instanceId},
        {"createdAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
    if (!writePrivateJson(path, markers)) { error = "COMMAND_UNAVAILABLE"; return false; }
    return true;
}
bool TargetGuard::release(const QString& fingerprint, const QString& executionId)
{
    QLockFile lock(QDir(_directory).filePath("targets.lock"));
    if (!lock.tryLock(0))
        return false;
    const auto path = QDir(_directory).filePath("targets.json");
    auto stored = readJson(path, 128 * 1024);
    if (!stored || stored->value(fingerprint).toObject().value("executionId").toString() != executionId)
        return false;
    stored->remove(fingerprint);
    return writePrivateJson(path, *stored);
}
QJsonObject TargetGuard::markers() const
{
    return readJson(QDir(_directory).filePath("targets.json"), 128 * 1024).value_or(QJsonObject{});
}
bool TargetGuard::acknowledge(const QString& fingerprint)
{
    const auto execution = markers().value(fingerprint).toObject().value("executionId").toString();
    return !execution.isEmpty() && release(fingerprint, execution);
}
}
