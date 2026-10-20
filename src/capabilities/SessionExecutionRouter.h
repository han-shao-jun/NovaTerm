#pragma once
#include "TargetEnvironment.h"
namespace NovaTerm::Capabilities {
enum class TargetTransport { Local, Ssh, Serial, Telnet, Custom };
enum class ExecutionRoute { Isolated, SharedTerminal };
enum class RouteUnavailable { None, DiagnosticPermissionRequired, UnknownEnvironment,
    EnvironmentMismatch, EvidenceExpired, NotReady, ConsentRequired, BackendUnavailable,
    TerminalOccupied, FileTransferBusy };
struct RecipeRequirements {
    CapabilityId id;
    EnvironmentKind environment{EnvironmentKind::Linux};
    ExecutionRoute route{ExecutionRoute::Isolated};
    QString planDigest;
};
struct RecipeAuthorization {
    PrincipalId principal;
    bool diagnosticExec{false};
    bool fullTerminalInput{false}; // Never substitutes for DiagnosticExec.
    bool sharedOccupancyConsent{false};
};
struct TargetFacts {
    TargetTransport transport{TargetTransport::Custom};
    EnvironmentSnapshot environment;
    bool isolatedAvailable{false}, sharedAvailable{false};
    bool tuiOrReplOrLogin{false}, fileTransfer{false}, occupied{false};
};
struct RouteDecision {
    std::optional<ExecutionRoute> route;
    RouteUnavailable reason{RouteUnavailable::None};
    quint64 environmentRevision{0};
    QString readinessEvidenceId;
};
class SessionExecutionRouter
{
public:
    static RouteDecision resolve(const TargetFacts&, const RecipeRequirements&, const RecipeAuthorization&, TimePoint now);
};
}
