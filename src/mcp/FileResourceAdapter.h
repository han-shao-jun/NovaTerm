#pragma once
#include "McpAccess.h"
#include "contracts/TransferBinding.h"
namespace NovaTerm::Mcp {
// Resolves existing SSH connection facts without creating a connection.
class FileResourceAdapter final : public QObject {
public:
    FileResourceAdapter(SessionDirectory&, QObject* parent);
    std::optional<SessionBinding> liveBinding(const QString& session);
private:
    SessionDirectory& _sessions;
};
}
