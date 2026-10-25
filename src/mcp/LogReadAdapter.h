#pragma once
#include "mcp/contracts/ToolRequests.h"
#include "mcp/contracts/Errors.h"
#include <QJsonObject>
class SessionDirectory;
namespace NovaTerm::Mcp {
class AccessStore;
[[nodiscard]] Result<QJsonObject> readLogContext(AccessStore&, SessionDirectory&,
    const QString& instance, const QString& client, const QByteArray& key, const ReadContextRequest&);
}
