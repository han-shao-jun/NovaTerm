/** @file CapabilityAdapter.cpp @brief Session 事实到能力模型的唯一转换实现。 */
#include "CapabilityAdapter.h"
#include "McpAccess.h"
#include "session/ISessionScriptProvider.h"
#include "session/SessionCommandCoordinator.h"
#include "session/SessionCommandFacade.h"
#include "session/SessionInputArbiter.h"
#include "session/SessionTerminalController.h"

namespace NovaTerm::Mcp {

CapabilityFacts capabilityFacts(const SessionDirectory::Entry& entry,
                                const AccessStore& access)
{
    CapabilityFacts facts;
    facts.sessionRunning = entry.state == SessionState::Running
        && entry.session && entry.session->state() == SessionState::Running;
    facts.transportConnected = entry.transport && entry.transport->isConnected();
    facts.transportMatchesSession = entry.session
        && entry.session->transport() == entry.transport;
    facts.generationMatches = entry.session
        && entry.session->statistics().generation == entry.generation;
    facts.targetFingerprintPresent = !entry.targetFingerprint.isEmpty();

    auto* facade = entry.session ? entry.session->commandFacade() : nullptr;
    const auto profile = facade ? facade->profile() : CommandPlatformProfile{};
    facts.diagnosticsExecutorAvailable = facade && facade->isAvailable()
        && profile.isAvailable();
    for (const auto& id : profile.supportedCommandIds())
        facts.supportedDiagnostics.insert(DiagnosticId{id});

    auto* coordinator = entry.session ? entry.session->commandCoordinator() : nullptr;
    facts.unverifiedPrompt = entry.kind == TransportKind::Ssh
        && coordinator && coordinator->allowsUnverifiedPrompt();
    facts.idleCompletion = entry.kind == TransportKind::Serial
        && coordinator && coordinator->allowsIdleCompletion();
    const bool trustedLocal = entry.kind == TransportKind::LocalShell
        && coordinator && coordinator->hasTrustedProfile()
        && coordinator->isPromptReady();
    facts.interactiveReachable = facade && facade->isAvailable()
        && !entry.targetFingerprint.isEmpty()
        && (facade->capabilities().mode == CommandExecutionMode::InteractiveFramed
            || facts.unverifiedPrompt || facts.idleCompletion || trustedLocal);
    facts.scriptProviderAvailable = entry.session && entry.session->scriptProvider()
        && entry.session->scriptProvider()->isAvailable();

    facts.transferLease = entry.session
        && entry.session->inputArbiter()->hasTransferLease();
    facts.legacyTerminalBusy = entry.session
        && entry.session->inputArbiter()->hasMcpLease()
        && entry.session->terminalController()->owner().isEmpty();
    facts.terminalOwner = entry.session
        ? entry.session->terminalController()->owner() : QString{};
    facts.shellIntegrationAvailable = entry.session
        && entry.session->terminalController()->integrated();
    facts.elevationTargetVerified = access.elevationTargetIdentityVerified(entry);
    return facts;
}

CapabilitySnapshot sessionCapabilities(const SessionDirectory::Entry& entry,
    const QString& clientId, const AccessStore& access, bool humanConfirmation)
{
    CapabilityContext context;
    context.client = ClientId{clientId};
    context.humanConfirmation = humanConfirmation;
    context.credentialUpdatePending = access.credentialUpdatePending();
    return CapabilityResolver::resolve(
        access.grantSnapshot(context.client, entry),
        capabilityFacts(entry, access), context);
}

} // namespace NovaTerm::Mcp
