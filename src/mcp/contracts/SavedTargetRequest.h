#pragma once
#include <QString>
namespace NovaTerm::Mcp {
struct ListSavedTargetsRequest { int limit{25}; QString cursor; };
struct TargetConnectionRequest {
    QString targetId, configVersion, operationId;
    bool query{false};
};
}
