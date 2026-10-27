#pragma once
#include <QString>
namespace NovaTerm::Mcp {
struct ListSavedTargetsRequest { int limit{25}; QString cursor; };
enum class TargetConnectionAction { Open, Query, Close };
struct TargetConnectionRequest {
    QString targetId, configVersion, operationId;
    TargetConnectionAction action{TargetConnectionAction::Open};
    QString epoch;
};
}
