#pragma once
#include "McpAccess.h"
#include "contracts/TransferBinding.h"
namespace NovaTerm::Mcp {
// Resolves existing serial connection facts without opening a port.
class SerialFileTargetAdapter final : public QObject {
public:
    SerialFileTargetAdapter(SessionDirectory&, QObject* parent);
    std::optional<SerialFileBinding> liveBinding(const QString& session);
private:
    SessionDirectory& _sessions;
};
}
