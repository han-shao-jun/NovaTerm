/** @file CapabilityResolver.cpp @brief 能力/就绪规则的唯一实现。 */
#include "CapabilityResolver.h"

#include <algorithm>

namespace NovaTerm::Mcp {
namespace {

bool live(const CapabilityFacts& facts)
{
    return facts.sessionRunning && facts.transportConnected
        && facts.transportMatchesSession && facts.generationMatches;
}

QString terminalInputReason(const GrantSnapshot& grant, const CapabilityFacts& facts,
                            const CapabilityContext& context)
{
    if (!grant.selection.fullTerminalInput)
        return QStringLiteral("TERMINAL_PERMISSION_REQUIRED");
    if (!live(facts))
        return QStringLiteral("SESSION_NOT_READY");
    if (facts.transferLease)
        return QStringLiteral("FILE_TRANSFER_BUSY");
    if ((!facts.terminalOwner.isEmpty() && facts.terminalOwner != context.client.str())
        || facts.legacyTerminalBusy)
        return QStringLiteral("TERMINAL_BUSY");
    return {};
}

} // namespace

QString CapabilityResolver::elevationGrantDisabledReason(
    const CapabilityFacts& facts, const GrantSnapshot& grant, bool credentialPending)
{
    if (!live(facts))
        return QStringLiteral("SESSION_NOT_READY");
    if (!grant.selection.fullTerminalInput)
        return QStringLiteral("TERMINAL_PERMISSION_REQUIRED");
    if (credentialPending)
        return QStringLiteral("CREDENTIAL_UPDATE_IN_PROGRESS");
    return {};
}

QString CapabilityResolver::elevationExecutionUnavailableReason(bool targetVerified)
{
    return targetVerified ? QStringLiteral("ELEVATION_ADAPTER_UNAVAILABLE")
                          : QStringLiteral("TARGET_IDENTITY_UNVERIFIED");
}

CapabilitySnapshot CapabilityResolver::resolve(const GrantSnapshot& grant,
    const CapabilityFacts& facts, const CapabilityContext& context)
{
    CapabilitySnapshot snapshot;
    snapshot.permissions = grant.selection;
    snapshot.terminalInputDisabledReason = terminalInputReason(grant, facts, context);
    snapshot.terminalInputAvailable = snapshot.terminalInputDisabledReason.isEmpty();
    snapshot.terminalOwner = facts.terminalOwner;
    snapshot.ownedByThisClient = !facts.terminalOwner.isEmpty()
        && facts.terminalOwner == context.client.str();
    snapshot.shellIntegrationAvailable = facts.shellIntegrationAvailable;
    snapshot.diagnosticsReady = facts.sessionRunning
        && facts.diagnosticsExecutorAvailable && facts.targetFingerprintPresent;

    snapshot.capabilities = {QStringLiteral("read_context"),
        QStringLiteral("search_context"), QStringLiteral("list_commands")};
    if (snapshot.terminalInputAvailable)
        snapshot.capabilities.append(QStringLiteral("terminal_interaction"));
    if (context.humanConfirmation)
        snapshot.capabilities.append(QStringLiteral("human_confirmation"));
    const bool diagnosticGranted = std::any_of(
        grant.selection.diagnostics.cbegin(), grant.selection.diagnostics.cend(),
        [&facts](const DiagnosticId& id) { return facts.supportedDiagnostics.contains(id); });
    if (facts.diagnosticsExecutorAvailable && diagnosticGranted)
        snapshot.capabilities.append(QStringLiteral("execute_command"));
    if (facts.interactiveReachable && grant.selection.riskControlledCommand)
        snapshot.capabilities.append(QStringLiteral("run_command"));
    if (facts.interactiveReachable && !facts.unverifiedPrompt
        && grant.selection.script && facts.scriptProviderAvailable)
        snapshot.capabilities.append(QStringLiteral("run_script"));

    snapshot.elevationAvailable = false;
    snapshot.elevationDisabledReason =
        elevationExecutionUnavailableReason(facts.elevationTargetVerified);
    snapshot.elevationGrantDisabledReason = elevationGrantDisabledReason(
        facts, grant, context.credentialUpdatePending);
    snapshot.elevationGrantSelectable = snapshot.elevationGrantDisabledReason.isEmpty()
        || grant.selection.elevation;
    return snapshot;
}

} // namespace NovaTerm::Mcp
