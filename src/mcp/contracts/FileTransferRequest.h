#pragma once
#include "SessionBinding.h"
#include <optional>
#include <variant>
namespace NovaTerm::Mcp {
struct StartFileUploadRequest {
    SessionReference reference;
    QString localPath, remotePath, operationId;
    bool overwrite{false};
    QString confirmationToken{};
    std::optional<bool> confirmed{};
};
struct StartFileDownloadRequest {
    SessionReference reference;
    QString localPath, remotePath, operationId;
    bool overwrite{false};
};
struct QueryFileTransferRequest { QString transferId; };
struct CancelFileTransferRequest { QString transferId; };
using FileTransferRequest = std::variant<StartFileUploadRequest, QueryFileTransferRequest, CancelFileTransferRequest, StartFileDownloadRequest>;
struct StartSerialFileUploadRequest {
    SessionReference reference;
    QString localPath, remoteName, operationId;
    QString confirmationToken{};
    std::optional<bool> confirmed{};
};
struct QuerySerialFileTransferRequest { QString transferId; };
struct CancelSerialFileTransferRequest { QString transferId; };
using SerialFileTransferRequest = std::variant<StartSerialFileUploadRequest, QuerySerialFileTransferRequest, CancelSerialFileTransferRequest>;
}
