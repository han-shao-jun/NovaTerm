#pragma once
#include "SessionBinding.h"
namespace NovaTerm::Mcp {
struct ClearRequest {
    SessionId session;
    Epoch epoch;
    OperationId operation;
    bool all{false};
    quint64 expectedGeneration{0};
    QString clearToken;
};
}
