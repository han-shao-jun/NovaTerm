#pragma once
#include "capabilities/TypedCapabilityPort.h"
#include "mcp/application/ObservationState.h"
namespace NovaTerm::Capabilities::Terminal {
struct InputSubmission {
    Mcp::InteractTerminalRequest request;
    Mcp::InputClaims claims;
    bool allowReplay{true};
    /** @brief 仅通知本次新输入已受理，不把交付校验失败等同于没有提交。 */
    std::function<void()> submitted;
};
struct ElevationSubmission { Mcp::RequestElevationRequest request; Mcp::InputClaims claims; };
class TerminalInputPort
{
public:
    virtual ~TerminalInputPort() = default;
    virtual Mcp::TerminalControlFacts facts(const Mcp::ClientId&, const Mcp::SessionReference&) = 0;
    virtual bool acquire(const Mcp::ClientId&, const Mcp::SessionReference&) = 0;
    virtual bool submit(const Mcp::ClientId&, const Mcp::InteractTerminalRequest&) = 0;
    virtual void release(const Mcp::ClientId&, const Mcp::InteractTerminalRequest&) = 0;
    virtual bool canElevate(const Mcp::ClientId&, const Mcp::SessionReference&) = 0;
};
class InputCapabilities
{
public:
    InputCapabilities(CapabilityDispatcher&, Mcp::ObservationState&, TerminalInputPort&,
        std::function<bool()> ownerAlive = {});
    TypedCapabilityPort<InputSubmission, Mcp::Result<Mcp::InputAdmission>> input;
    TypedCapabilityPort<ElevationSubmission, Mcp::Result<std::monostate>> elevation;
};
}
