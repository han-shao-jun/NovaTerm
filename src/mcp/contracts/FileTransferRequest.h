#pragma once
#include "SessionBinding.h"
#include <variant>
namespace NovaTerm::Mcp {
struct StartFileUploadRequest {
    SessionReference reference;
    QString resourceId, resourceRevision, relativePath, scopeId, scopeRevision, remoteName, operationId;
};
struct QueryFileTransferRequest { QString transferId; };
struct CancelFileTransferRequest { QString transferId; };
using FileTransferRequest = std::variant<StartFileUploadRequest, QueryFileTransferRequest, CancelFileTransferRequest>;
struct StartSerialFileUploadRequest {
    SessionReference reference;
    QString resourceId, resourceRevision, relativePath, targetId, targetRevision, remoteName, operationId;
};
struct QuerySerialFileTransferRequest { QString transferId; };
struct CancelSerialFileTransferRequest { QString transferId; };
using SerialFileTransferRequest = std::variant<StartSerialFileUploadRequest, QuerySerialFileTransferRequest, CancelSerialFileTransferRequest>;
}
