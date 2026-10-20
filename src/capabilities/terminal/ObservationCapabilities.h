#pragma once
#include "capabilities/TypedCapabilityPort.h"
#include "mcp/application/ObservationState.h"
namespace NovaTerm::Capabilities::Terminal {
// ObservationState owns wait policy/replay. This owner holds only admission and delivery.
class ObservationCapabilities final
{
public:
    using Port = TypedCapabilityPort<Mcp::ObservationWait, Mcp::ObservationCompletion>;
    ObservationCapabilities(CapabilityDispatcher&, Mcp::ObservationState&);
    ~ObservationCapabilities();
    ObservationCapabilities(const ObservationCapabilities&) = delete;
    ObservationCapabilities& operator=(const ObservationCapabilities&) = delete;
    void complete(Mcp::ObservationCompletion);
    void cancel(const Mcp::RequestId&);
private:
    struct State;
    std::shared_ptr<State> _state;
public:
    Port read, input, elevation;
};
}
