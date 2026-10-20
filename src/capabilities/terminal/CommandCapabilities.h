#pragma once
#include "capabilities/TypedCapabilityPort.h"
#include "mcp/tasks/CommandRuntime.h"
#include <map>
namespace NovaTerm::Capabilities::Terminal {
// Terminal execution domain envelope. Runtime still owns all jobs/confirmations/guards.
class CommandCapabilities final
{
public:
    using Response = std::optional<Mcp::CommandResult>;
    using Port = TypedCapabilityPort<Mcp::CommandInvocation, Response>;
    CommandCapabilities(CapabilityDispatcher&, Mcp::CommandRuntime&);
    ~CommandCapabilities();
    CommandCapabilities(const CommandCapabilities&) = delete;
    CommandCapabilities& operator=(const CommandCapabilities&) = delete;
    void complete(const Mcp::TaskId&, Response);
    void cancel(const Mcp::TaskId&);
    void disconnect(const QString& peer);
    void stop();
    void deadline(const Mcp::TaskId&, TimePoint);
    Port diagnostic, command, script;
private:
    void start(Mcp::CommandInvocation, const CapabilityId&, InvocationLease, Completion<Response>);
    struct Active {
        InvocationLease lease;
        Completion<Response> done;
        Mcp::PeerId peer;
    };
    Mcp::CommandRuntime& _runtime;
    std::map<QString, Active> _active;
};
}
