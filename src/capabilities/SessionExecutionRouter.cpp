#include "SessionExecutionRouter.h"
namespace NovaTerm::Capabilities {
RouteDecision SessionExecutionRouter::resolve(const TargetFacts& facts, const RecipeRequirements& recipe,
    const RecipeAuthorization& authorization, TimePoint now)
{
    const auto& environment = facts.environment;
    const auto deny = [](RouteUnavailable reason) { return RouteDecision{std::nullopt, reason, 0, {}}; };
    if (!authorization.diagnosticExec) return deny(RouteUnavailable::DiagnosticPermissionRequired);
    if (environment.kind == EnvironmentKind::Unknown) return deny(RouteUnavailable::UnknownEnvironment);
    if (environment.kind != recipe.environment
        && !(recipe.environment == EnvironmentKind::Posix && environment.kind == EnvironmentKind::Linux))
        return deny(RouteUnavailable::EnvironmentMismatch);
    if (now < environment.observedAt || now >= environment.validUntil
        || environment.validUntil - environment.observedAt > std::chrono::seconds(5))
        return deny(RouteUnavailable::EvidenceExpired);
    if (recipe.route == ExecutionRoute::Isolated) {
        if (!facts.isolatedAvailable) return deny(RouteUnavailable::BackendUnavailable);
    } else {
        if (!facts.sharedAvailable) return deny(RouteUnavailable::BackendUnavailable);
        if (facts.fileTransfer) return deny(RouteUnavailable::FileTransferBusy);
        if (facts.occupied || facts.tuiOrReplOrLogin) return deny(RouteUnavailable::TerminalOccupied);
        if (!environment.ready || environment.readinessEvidenceId.isEmpty()) return deny(RouteUnavailable::NotReady);
        if (environment.source == EnvironmentSource::LocalConfirmation) {
            if (!environment.oneSubmission || environment.consentPrincipal != authorization.principal
                || environment.consentRecipe != recipe.id || environment.planDigest != recipe.planDigest)
                return deny(RouteUnavailable::ConsentRequired);
        } else if (environment.source != EnvironmentSource::PairedAdapter || !authorization.sharedOccupancyConsent) {
            return deny(RouteUnavailable::ConsentRequired);
        }
    }
    return {recipe.route, RouteUnavailable::None, environment.revision, environment.readinessEvidenceId};
}
}
