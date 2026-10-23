#pragma once
#include "mcp/contracts/HistoryToolRequest.h"
#include "mcp/contracts/Errors.h"
#include <QJsonArray>
namespace NovaTerm::Mcp {
class HistoryToolCodec final {
public:
    static const QJsonArray& catalog();
    static bool known(const QString&);
    static Result<HistoryToolRequest> decode(const QString&,const QJsonObject&);
};
}
