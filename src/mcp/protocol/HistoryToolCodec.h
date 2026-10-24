#pragma once
#include "mcp/contracts/Errors.h"
#include "HistoryToolRequest.h"
#include <QJsonArray>
#include <QJsonObject>
namespace NovaTerm::Mcp {
// Protocol-only history extension. No storage, authorization or Session dependencies.
class HistoryToolCodec final {
public:
    static QJsonArray catalog();
    static bool known(const QString&);
    static QString validate(const QString&, const QJsonObject&);
    static Result<HistoryToolRequest> decode(const QString&, const QJsonObject&);
    static QString toolName(const HistoryToolRequest&);
};
}
