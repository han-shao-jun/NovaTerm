/**
 * @file AuthorizationTests.cpp
 * @brief 单项授权事务：绑定/版本拒绝、级联、重连语义与版本推进。
 */
#include "mcp/application/AuthorizationService.h"
#include "mcp/application/CapabilityResolver.h"
#include "mcp/application/CaptureSearch.h"
#include "mcp/application/CaptureStore.h"
#include "mcp/application/SnapshotStore.h"
#include "mcp/application/CommandDedup.h"
#include "mcp/application/RequestScheduler.h"
#include "mcp/tasks/ConfirmationRegistry.h"
#include "mcp/contracts/Permissions.h"
#include "mcp/contracts/TaskPhases.h"
#include "mcp/security/ClientRegistry.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTest>

#include <functional>

using namespace NovaTerm::Mcp;

namespace {

struct FakePorts
{
    bool enabled{true};
    bool credentialPending{false};
    QSet<QString> clients{QStringLiteral("client-1")};
    quint64 version{3};

    AuthorizationPorts ports()
    {
        return AuthorizationPorts{
            [this] { return enabled; },
            [this](const ClientId& id) { return clients.contains(id.str()); },
            [this](const ClientId&) { return ClientAccessVersion{version}; },
            [this](const ClientId&) { return ClientAccessVersion{++version}; },
            [](const DiagnosticId& id) {
                return id.str() == QLatin1String("os.uname")
                    || id.str() == QLatin1String("system.uptime");
            },
            [this] { return credentialPending; },
        };
    }
};

SessionBinding binding(const QString& session = QStringLiteral("session-1"),
                       const QString& attachment = QStringLiteral("attach-1"),
                       const QString& epoch = QStringLiteral("epoch-1"),
                       quint64 generation = 5)
{
    SessionBinding value;
    value.session = SessionId{session};
    value.attachment = AttachmentId{attachment};
    value.epoch = Epoch{epoch};
    value.generation = Generation{generation};
    value.targetFingerprint = QStringLiteral("fingerprint-1");
    return value;
}

GrantFacts facts(const SessionBinding& value = binding(), bool running = true)
{
    GrantFacts result;
    result.binding = value;
    result.sessionRunning = running;
    result.transportConnected = running;
    result.transportMatchesSession = running;
    result.generationMatches = running;
    return result;
}

PermissionChange change(PermissionKind kind, bool selected,
                        GrantRevision revision = GrantRevision{0},
                        std::optional<DiagnosticId> diagnostic = std::nullopt,
                        SessionBinding value = binding())
{
    PermissionChange result;
    result.client = ClientId{QStringLiteral("client-1")};
    result.binding = value;
    result.expectedRevision = revision;
    result.kind = kind;
    result.diagnostic = diagnostic;
    result.selected = selected;
    return result;
}

GrantSnapshot apply(AuthorizationService& service, const PermissionChange& request,
                    const GrantFacts& current)
{
    const auto result = service.change(request, current);
    if (isError(result))
        return errorIfAny(result)->current;
    return std::get<GrantSnapshot>(result);
}

} // namespace

class AuthorizationTests : public QObject
{
    Q_OBJECT
private slots:
    void confirmationRejectsChangedFactsAndReplay();
    void confirmationUserGenerationPreservesPresence();
    void taskPhasesAdvanceOnlyForward();

    void singleChangeAdvancesRowAndClientVersions();
    void clientIdentityGenerationIsIndependentAndNeverReused();
    void staleBindingAndRevisionAreRejected();
    void staleBindingRejectsEveryIdentityChange();
    void clientRowsNeverCopyAnotherClientsSelections();
    void readRevocationClearsExecutionPermissions();
    void fullInputImpliesReadAndRequiresLiveSession();
    void elevationRequiresFullInputAndIdleCredentials();
    void diagnosticsMustBeKnownAndRequireRead();
    void reconnectHidesExecutionButKeepsRead();
    void clearTerminalRequiresIndependentLiveReadGrant();
    void clearTerminalRejectsStaleRowsAndResetsWithBinding();
    void disabledOrUnknownClientIsRejected();
    void capabilitiesFollowGrantsAndFacts();
    void terminalInputReasonOrderIsStable();
    void elevationReasonSeparatesGrantFromExecution();
    void diagnosticsReadinessRequiresBackendAndTarget();
    void captureScopesKeepSeparateQuotas();
    void capturesExpireAndFilterByOwner();
    void snapshotStoreCoalescesWithinWindow();
    void snapshotStoreReloadsOnEpochChange();
    void captureSearchScopesAndLimits();
    void captureSearchFindsPatternPastChunkBoundary();
    void confirmationsKeepNonceAndPendingState();
    void requestSchedulerEnforcesBudgetsAndCancellation();
    void commandDedupBlocksUntilSwept();
};

void AuthorizationTests::clientIdentityGenerationIsIndependentAndNeverReused()
{
    ClientRegistry clients;
    const ClientId id{"client-1"};
    QCOMPARE(clients.identityVersion(id), quint64(0));
    clients.insert({id, "client", QByteArray(32, 'a'), false, ClientAccessVersion{1}});
    const auto original = clients.identityVersion(id);
    QVERIFY(original > 0);
    static_cast<void>(clients.advance(id));
    static_cast<void>(clients.advance(id));
    QCOMPARE(clients.version(id).value(), quint64(3));
    QCOMPARE(clients.identityVersion(id), original);
    clients.replaceAll(clients.clients());
    QCOMPARE(clients.identityVersion(id), original);
    QVERIFY(clients.updateDigest(id, QByteArray(32, 'b'), false));
    const auto rotated = clients.identityVersion(id);
    QVERIFY(rotated > original);
    QVERIFY(clients.remove(id));
    QCOMPARE(clients.identityVersion(id), quint64(0));
    clients.insert({id, "client", QByteArray(32, 'b'), false, ClientAccessVersion{1}});
    const auto recreated = clients.identityVersion(id);
    QVERIFY(recreated > rotated);
    const auto accessVersion = clients.version(id);
    clients.advanceAllIdentities();
    QCOMPARE(clients.version(id), accessVersion);
    QVERIFY(clients.identityVersion(id) > recreated);
    const auto stopped = clients.identityVersion(id);
    clients.clear();
    clients.insert({id, "client", QByteArray(32, 'b'), false, ClientAccessVersion{1}});
    QVERIFY(clients.identityVersion(id) > stopped);
}

void AuthorizationTests::singleChangeAdvancesRowAndClientVersions()
{
    FakePorts ports;
    AuthorizationService service{ports.ports()};
    const auto current = facts();
    const auto applied = apply(service, change(PermissionKind::ReadOutput, true), current);
    QVERIFY(applied.selection.read);
    QCOMPARE(applied.revision.value(), 1u);
    QCOMPARE(applied.accessVersion.value(), ports.version);
    QCOMPARE(applied.binding.epoch.str(), QStringLiteral("epoch-1"));

    const auto risk = apply(service,
        change(PermissionKind::RiskControlledTerminalCommand, true, GrantRevision{1}),
        current);
    QCOMPARE(risk.revision.value(), 2u);
    QVERIFY(risk.selection.read);
    QVERIFY(risk.selection.riskControlledCommand);
    QCOMPARE(ports.version, quint64(5));
}

void AuthorizationTests::clearTerminalRequiresIndependentLiveReadGrant()
{
    FakePorts ports;
    ports.clients.insert(QStringLiteral("client-2"));
    AuthorizationService service{ports.ports()};
    const auto current = facts();
    QVERIFY(!PermissionSelection{}.clearTerminal);
    auto changed = PermissionSelection{};
    changed.clearTerminal = true;
    QVERIFY(changed != PermissionSelection{});
    const auto withoutRead = service.change(change(PermissionKind::ClearTerminal, true), current);
    QVERIFY(isError(withoutRead));
    QCOMPARE(errorIfAny(withoutRead)->reason, AuthorizationRejection::InvalidSelection);
    const auto input = apply(service, change(PermissionKind::FullTerminalInput, true), current);
    QVERIFY(!input.selection.clearTerminal);
    const auto unavailable = service.change(change(PermissionKind::ClearTerminal, true,
        input.revision), facts(binding(), false));
    QVERIFY(isError(unavailable));
    QCOMPARE(errorIfAny(unavailable)->reason, AuthorizationRejection::SessionNotReady);
    const auto clear = apply(service, change(PermissionKind::ClearTerminal, true,
        input.revision), current);
    QVERIFY(clear.selection.clearTerminal);
    QVERIFY(!service.snapshot(ClientId{QStringLiteral("client-2")}, current).selection.clearTerminal);
    const auto inputRevoked = apply(service, change(PermissionKind::FullTerminalInput, false,
        clear.revision), current);
    QVERIFY(inputRevoked.selection.clearTerminal);
    const auto readRevoked = apply(service, change(PermissionKind::ReadOutput, false,
        inputRevoked.revision), current);
    QVERIFY(!readRevoked.selection.clearTerminal);
    const auto readAgain = apply(service, change(PermissionKind::ReadOutput, true,
        readRevoked.revision), current);
    QVERIFY(!readAgain.selection.clearTerminal);
}

void AuthorizationTests::clearTerminalRejectsStaleRowsAndResetsWithBinding()
{
    FakePorts ports;
    AuthorizationService service{ports.ports()};
    const auto current = facts();
    const auto read = apply(service, change(PermissionKind::ReadOutput, true), current);
    const auto clear = apply(service, change(PermissionKind::ClearTerminal, true,
        read.revision), current);
    const auto staleRevision = service.change(change(PermissionKind::ClearTerminal, false,
        read.revision), current);
    QVERIFY(isError(staleRevision));
    QCOMPARE(errorIfAny(staleRevision)->reason, AuthorizationRejection::StaleRevision);
    QVERIFY(errorIfAny(staleRevision)->current.selection.clearTerminal);
    auto reconnected = current;
    reconnected.binding.epoch = Epoch{QStringLiteral("epoch-2")};
    const auto view = service.snapshot(ClientId{QStringLiteral("client-1")}, reconnected);
    QVERIFY(view.selection.read);
    QVERIFY(!view.selection.clearTerminal);
    const auto staleBinding = service.change(change(PermissionKind::ClearTerminal, true,
        clear.revision), reconnected);
    QVERIFY(isError(staleBinding));
    QCOMPARE(errorIfAny(staleBinding)->reason, AuthorizationRejection::StaleBinding);
    const auto rebound = apply(service, change(PermissionKind::ReadOutput, true,
        clear.revision, std::nullopt, reconnected.binding), reconnected);
    QVERIFY(!rebound.selection.clearTerminal);
    auto replaced = reconnected;
    replaced.binding.attachment = AttachmentId{QStringLiteral("replacement")};
    QVERIFY(!service.snapshot(ClientId{QStringLiteral("client-1")}, replaced).selection.clearTerminal);
}

void AuthorizationTests::staleBindingAndRevisionAreRejected()
{
    FakePorts ports;
    AuthorizationService service{ports.ports()};
    const auto current = facts();
    apply(service, change(PermissionKind::ReadOutput, true), current);

    const auto staleBinding = service.change(
        change(PermissionKind::ScriptPublishAndInvoke, true, GrantRevision{1},
            std::nullopt, binding(QStringLiteral("session-1"), QStringLiteral("old-attach"))),
        current);
    QVERIFY(isError(staleBinding));
    QCOMPARE(errorIfAny(staleBinding)->reason, AuthorizationRejection::StaleBinding);
    QVERIFY(errorIfAny(staleBinding)->current.selection.read);

    const auto staleRevision = service.change(
        change(PermissionKind::ScriptPublishAndInvoke, true, GrantRevision{0}), current);
    QVERIFY(isError(staleRevision));
    QCOMPARE(errorIfAny(staleRevision)->reason, AuthorizationRejection::StaleRevision);
    QCOMPARE(errorIfAny(staleRevision)->current.revision.value(), 1u);
}

void AuthorizationTests::staleBindingRejectsEveryIdentityChange()
{
    FakePorts ports;
    AuthorizationService service{ports.ports()};
    const auto original = facts();
    const auto selected = apply(service, change(PermissionKind::FullTerminalInput, true), original);
    const auto version = ports.version;
    const std::vector<std::function<void(SessionBinding&)>> mutate{
        [](auto& value) { value.session = SessionId{QStringLiteral("other-session")}; },
        [](auto& value) { value.attachment = AttachmentId{QStringLiteral("other-attachment")}; },
        [](auto& value) { value.epoch = Epoch{QStringLiteral("other-epoch")}; },
        [](auto& value) { value.generation = Generation{6}; },
        [](auto& value) { value.targetFingerprint = QStringLiteral("other-target"); },
    };
    for (const auto& changeBinding : mutate) {
        auto current = original;
        changeBinding(current.binding);
        const auto rejected = service.change(change(PermissionKind::FullTerminalInput, false,
            selected.revision, std::nullopt, original.binding), current);
        QVERIFY(isError(rejected));
        QCOMPARE(errorIfAny(rejected)->reason, AuthorizationRejection::StaleBinding);
        QVERIFY(!errorIfAny(rejected)->current.selection.fullTerminalInput);
        QCOMPARE(ports.version, version);
    }
    QVERIFY(service.snapshot(ClientId{QStringLiteral("client-1")}, original).selection.fullTerminalInput);
}

void AuthorizationTests::clientRowsNeverCopyAnotherClientsSelections()
{
    FakePorts ports;
    ports.clients.insert(QStringLiteral("client-2"));
    AuthorizationService service{ports.ports()};
    const auto current = facts();
    const auto first = apply(service, change(PermissionKind::FullTerminalInput, true), current);
    auto second = change(PermissionKind::ReadOutput, true, first.revision);
    second.client = ClientId{QStringLiteral("client-2")};
    const auto stale = service.change(second, current);
    QVERIFY(isError(stale));
    QCOMPARE(errorIfAny(stale)->reason, AuthorizationRejection::StaleRevision);
    QVERIFY(!errorIfAny(stale)->current.selection.read);
    second.expectedRevision = GrantRevision{0};
    const auto read = apply(service, second, current);
    QVERIFY(read.selection.read);
    QVERIFY(!read.selection.fullTerminalInput && !read.selection.elevation);
    second.selected = false;
    second.expectedRevision = read.revision;
    QVERIFY(!apply(service, second, current).selection.read);
    const auto unchanged = service.snapshot(ClientId{QStringLiteral("client-1")}, current);
    QVERIFY(unchanged.selection.fullTerminalInput);
    QCOMPARE(unchanged.revision, first.revision);
}

void AuthorizationTests::readRevocationClearsExecutionPermissions()
{
    FakePorts ports;
    AuthorizationService service{ports.ports()};
    const auto current = facts();
    apply(service, change(PermissionKind::ReadOutput, true), current);
    apply(service, change(PermissionKind::ScriptPublishAndInvoke, true, GrantRevision{1}), current);
    apply(service, change(PermissionKind::RiskControlledTerminalCommand, true,
        GrantRevision{2}), current);
    apply(service, change(PermissionKind::FullTerminalInput, true, GrantRevision{3}), current);
    apply(service, change(PermissionKind::ManagedElevation, true, GrantRevision{4}), current);
    const auto diagnostic = apply(service, change(PermissionKind::Diagnostic, true, GrantRevision{5},
        DiagnosticId{QStringLiteral("os.uname")}), current);
    QVERIFY(!diagnostic.selection.diagnostics.isEmpty());
    const auto revoked = apply(service,
        change(PermissionKind::ReadOutput, false, GrantRevision{6}), current);
    QVERIFY(!revoked.selection.read);
    QVERIFY(revoked.selection.diagnostics.isEmpty());
    QVERIFY(!revoked.selection.riskControlledCommand);
    QVERIFY(!revoked.selection.script);
    QVERIFY(!revoked.selection.fullTerminalInput);
    QVERIFY(!revoked.selection.elevation);
    QCOMPARE(revoked.revision.value(), 7u);
}

void AuthorizationTests::fullInputImpliesReadAndRequiresLiveSession()
{
    FakePorts ports;
    AuthorizationService service{ports.ports()};
    const auto applied = apply(service,
        change(PermissionKind::FullTerminalInput, true), facts());
    QVERIFY(applied.selection.read);
    QVERIFY(applied.selection.fullTerminalInput);
    QCOMPARE(applied.revision.value(), 1u);

    const auto notReady = service.change(
        change(PermissionKind::FullTerminalInput, true, GrantRevision{1}),
        facts(binding(), false));
    QVERIFY(isError(notReady));
    QCOMPARE(errorIfAny(notReady)->reason, AuthorizationRejection::SessionNotReady);
}

void AuthorizationTests::elevationRequiresFullInputAndIdleCredentials()
{
    FakePorts ports;
    AuthorizationService service{ports.ports()};
    const auto current = facts();
    const auto withoutInput = service.change(
        change(PermissionKind::ManagedElevation, true), current);
    QVERIFY(isError(withoutInput));
    QCOMPARE(errorIfAny(withoutInput)->reason, AuthorizationRejection::InvalidSelection);

    apply(service, change(PermissionKind::FullTerminalInput, true), current);
    ports.credentialPending = true;
    const auto pending = service.change(
        change(PermissionKind::ManagedElevation, true, GrantRevision{1}), current);
    QVERIFY(isError(pending));
    QCOMPARE(errorIfAny(pending)->reason, AuthorizationRejection::CredentialsPending);

    ports.credentialPending = false;
    const auto granted = apply(service,
        change(PermissionKind::ManagedElevation, true, GrantRevision{1}), current);
    QVERIFY(granted.selection.elevation);
    const auto revokedInput = apply(service,
        change(PermissionKind::FullTerminalInput, false, GrantRevision{2}), current);
    QVERIFY(!revokedInput.selection.elevation);
    QVERIFY(revokedInput.selection.read);
}

void AuthorizationTests::diagnosticsMustBeKnownAndRequireRead()
{
    FakePorts ports;
    AuthorizationService service{ports.ports()};
    const auto current = facts();
    const auto beforeRead = service.change(
        change(PermissionKind::Diagnostic, true, GrantRevision{0},
            DiagnosticId{QStringLiteral("os.uname")}), current);
    QVERIFY(isError(beforeRead));
    QCOMPARE(errorIfAny(beforeRead)->reason, AuthorizationRejection::InvalidSelection);

    apply(service, change(PermissionKind::ReadOutput, true), current);
    const auto unknown = service.change(
        change(PermissionKind::Diagnostic, true, GrantRevision{1},
            DiagnosticId{QStringLiteral("file.delete")}), current);
    QVERIFY(isError(unknown));
    QCOMPARE(errorIfAny(unknown)->reason, AuthorizationRejection::InvalidSelection);

    const auto known = apply(service,
        change(PermissionKind::Diagnostic, true, GrantRevision{1},
            DiagnosticId{QStringLiteral("os.uname")}), current);
    QVERIFY(known.selection.diagnostics.contains(DiagnosticId{QStringLiteral("os.uname")}));
}

void AuthorizationTests::reconnectHidesExecutionButKeepsRead()
{
    FakePorts ports;
    AuthorizationService service{ports.ports()};
    const auto original = facts();
    apply(service, change(PermissionKind::ReadOutput, true), original);
    apply(service, change(PermissionKind::ScriptPublishAndInvoke, true, GrantRevision{1}),
        original);
    apply(service, change(PermissionKind::FullTerminalInput, true, GrantRevision{2}), original);

    const auto reconnected = facts(binding(QStringLiteral("session-1"),
        QStringLiteral("attach-1"), QStringLiteral("epoch-2"), 6));
    const auto view = service.snapshot(ClientId{QStringLiteral("client-1")}, reconnected);
    QVERIFY(view.selection.read);
    QVERIFY(!view.selection.script);
    QVERIFY(!view.selection.fullTerminalInput);
    QCOMPARE(view.revision.value(), 3u);

    // 新连接上的显式读取修改会重绑定，并保持执行类权限仍为未授权。
    const auto rebound = apply(service,
        change(PermissionKind::ReadOutput, true, view.revision, std::nullopt,
            reconnected.binding), reconnected);
    QVERIFY(rebound.selection.read);
    QVERIFY(!rebound.selection.script);
    QCOMPARE(rebound.revision.value(), 4u);
}

void AuthorizationTests::disabledOrUnknownClientIsRejected()
{
    FakePorts ports;
    AuthorizationService service{ports.ports()};
    const auto current = facts();
    ports.enabled = false;
    const auto disabled = service.change(change(PermissionKind::ReadOutput, true), current);
    QVERIFY(isError(disabled));
    QCOMPARE(errorIfAny(disabled)->reason, AuthorizationRejection::Disabled);

    ports.enabled = true;
    auto unknown = change(PermissionKind::ReadOutput, true);
    unknown.client = ClientId{QStringLiteral("missing")};
    const auto rejected = service.change(unknown, current);
    QVERIFY(isError(rejected));
    QCOMPARE(errorIfAny(rejected)->reason, AuthorizationRejection::UnknownClient);
    QCOMPARE(service.size(), std::size_t(0));
}

void AuthorizationTests::capabilitiesFollowGrantsAndFacts()
{
    FakePorts ports;
    AuthorizationService service{ports.ports()};
    const auto current = facts();
    const auto read = apply(service, change(PermissionKind::ReadOutput, true), current);

    CapabilityFacts capabilityFacts;
    capabilityFacts.sessionRunning = true;
    capabilityFacts.transportConnected = true;
    capabilityFacts.transportMatchesSession = true;
    capabilityFacts.generationMatches = true;
    capabilityFacts.targetFingerprintPresent = true;
    capabilityFacts.diagnosticsExecutorAvailable = true;
    capabilityFacts.supportedDiagnostics.insert(DiagnosticId{QStringLiteral("os.uname")});
    capabilityFacts.interactiveReachable = true;
    capabilityFacts.scriptProviderAvailable = true;

    CapabilityContext context;
    context.client = ClientId{QStringLiteral("client-1")};
    auto snapshot = CapabilityResolver::resolve(read, capabilityFacts, context);
    QCOMPARE(snapshot.capabilities,
        QStringList({"read_context", "search_context", "list_commands"}));

    const auto withDiagnostic = apply(service,
        change(PermissionKind::Diagnostic, true, GrantRevision{1},
            DiagnosticId{QStringLiteral("os.uname")}), current);
    snapshot = CapabilityResolver::resolve(withDiagnostic, capabilityFacts, context);
    QVERIFY(snapshot.capabilities.contains(QStringLiteral("execute_command")));

    const auto risk = apply(service,
        change(PermissionKind::RiskControlledTerminalCommand, true, GrantRevision{2}), current);
    snapshot = CapabilityResolver::resolve(risk, capabilityFacts, context);
    QVERIFY(snapshot.capabilities.contains(QStringLiteral("run_command")));

    const auto script = apply(service,
        change(PermissionKind::ScriptPublishAndInvoke, true, GrantRevision{3}), current);
    snapshot = CapabilityResolver::resolve(script, capabilityFacts, context);
    QVERIFY(snapshot.capabilities.contains(QStringLiteral("run_script")));

    capabilityFacts.unverifiedPrompt = true;
    snapshot = CapabilityResolver::resolve(script, capabilityFacts, context);
    QVERIFY(!snapshot.capabilities.contains(QStringLiteral("run_script")));
    QVERIFY(snapshot.capabilities.contains(QStringLiteral("run_command")));
}

void AuthorizationTests::terminalInputReasonOrderIsStable()
{
    FakePorts ports;
    AuthorizationService service{ports.ports()};
    const auto current = facts();
    const auto read = apply(service, change(PermissionKind::ReadOutput, true), current);
    CapabilityFacts capabilityFacts;
    capabilityFacts.sessionRunning = true;
    capabilityFacts.transportConnected = true;
    capabilityFacts.transportMatchesSession = true;
    capabilityFacts.generationMatches = true;
    CapabilityContext context;
    context.client = ClientId{QStringLiteral("client-1")};

    auto snapshot = CapabilityResolver::resolve(read, capabilityFacts, context);
    QCOMPARE(snapshot.terminalInputDisabledReason,
        QStringLiteral("TERMINAL_PERMISSION_REQUIRED"));
    QVERIFY(!snapshot.terminalInputAvailable);

    const auto input = apply(service,
        change(PermissionKind::FullTerminalInput, true, GrantRevision{1}), current);
    capabilityFacts.transferLease = true;
    snapshot = CapabilityResolver::resolve(input, capabilityFacts, context);
    QCOMPARE(snapshot.terminalInputDisabledReason, QStringLiteral("FILE_TRANSFER_BUSY"));

    capabilityFacts.transferLease = false;
    capabilityFacts.terminalOwner = QStringLiteral("other-client");
    snapshot = CapabilityResolver::resolve(input, capabilityFacts, context);
    QCOMPARE(snapshot.terminalInputDisabledReason, QStringLiteral("TERMINAL_BUSY"));
    QVERIFY(!snapshot.ownedByThisClient);

    capabilityFacts.terminalOwner.clear();
    snapshot = CapabilityResolver::resolve(input, capabilityFacts, context);
    QVERIFY(snapshot.terminalInputAvailable);
    QVERIFY(snapshot.capabilities.contains(QStringLiteral("terminal_interaction")));

    capabilityFacts.transportConnected = false;
    snapshot = CapabilityResolver::resolve(input, capabilityFacts, context);
    QCOMPARE(snapshot.terminalInputDisabledReason, QStringLiteral("SESSION_NOT_READY"));
}

void AuthorizationTests::elevationReasonSeparatesGrantFromExecution()
{
    FakePorts ports;
    AuthorizationService service{ports.ports()};
    const auto current = facts();
    const auto read = apply(service, change(PermissionKind::ReadOutput, true), current);
    const auto input = apply(service,
        change(PermissionKind::FullTerminalInput, true, GrantRevision{1}), current);
    CapabilityFacts capabilityFacts;
    capabilityFacts.sessionRunning = true;
    capabilityFacts.transportConnected = true;
    capabilityFacts.transportMatchesSession = true;
    capabilityFacts.generationMatches = true;
    CapabilityContext context;
    context.client = ClientId{QStringLiteral("client-1")};

    auto snapshot = CapabilityResolver::resolve(read, capabilityFacts, context);
    QCOMPARE(snapshot.elevationGrantDisabledReason,
        QStringLiteral("TERMINAL_PERMISSION_REQUIRED"));
    QVERIFY(!snapshot.elevationGrantSelectable);

    snapshot = CapabilityResolver::resolve(input, capabilityFacts, context);
    QVERIFY(snapshot.elevationGrantSelectable);
    QVERIFY(!snapshot.elevationAvailable);
    QCOMPARE(snapshot.elevationDisabledReason, QStringLiteral("TARGET_IDENTITY_UNVERIFIED"));

    capabilityFacts.elevationTargetVerified = true;
    context.credentialUpdatePending = true;
    snapshot = CapabilityResolver::resolve(input, capabilityFacts, context);
    QCOMPARE(snapshot.elevationGrantDisabledReason,
        QStringLiteral("CREDENTIAL_UPDATE_IN_PROGRESS"));
    QCOMPARE(snapshot.elevationDisabledReason, QStringLiteral("ELEVATION_ADAPTER_UNAVAILABLE"));
}

void AuthorizationTests::diagnosticsReadinessRequiresBackendAndTarget()
{
    FakePorts ports;
    AuthorizationService service{ports.ports()};
    const auto grant = apply(service, change(PermissionKind::ReadOutput, true), facts());
    CapabilityFacts capabilityFacts;
    capabilityFacts.sessionRunning = true;
    capabilityFacts.diagnosticsExecutorAvailable = true;
    capabilityFacts.targetFingerprintPresent = false;
    CapabilityContext context;
    context.client = ClientId{QStringLiteral("client-1")};
    auto snapshot = CapabilityResolver::resolve(grant, capabilityFacts, context);
    QVERIFY(!snapshot.diagnosticsReady);

    capabilityFacts.targetFingerprintPresent = true;
    capabilityFacts.diagnosticsExecutorAvailable = false;
    snapshot = CapabilityResolver::resolve(grant, capabilityFacts, context);
    QVERIFY(!snapshot.diagnosticsReady);

    capabilityFacts.diagnosticsExecutorAvailable = true;
    snapshot = CapabilityResolver::resolve(grant, capabilityFacts, context);
    QVERIFY(snapshot.diagnosticsReady);
}

void AuthorizationTests::captureScopesKeepSeparateQuotas()
{
    CaptureStore store;
    auto record = [](CaptureScope scope, const QString& owner, const QString& session) {
        CaptureRecord value;
        value.scope = scope;
        value.owner = owner;
        value.session = session;
        value.expires = 1000;
        return value;
    };
    for (int i = 0; i < 5; ++i)
        store.put(QStringLiteral("connection-%1").arg(i),
            record(CaptureScope::Connection, QStringLiteral("conn"), QStringLiteral("s1")));
    QCOMPARE(store.size(CaptureScope::Connection, QStringLiteral("conn")), std::size_t(4));
    QVERIFY(!store.contains(QStringLiteral("connection-0")));
    QVERIFY(store.contains(QStringLiteral("connection-4")));

    for (int i = 0; i < 5; ++i)
        store.put(QStringLiteral("client-%1").arg(i),
            record(CaptureScope::Client, QStringLiteral("client"), QStringLiteral("s1")));
    QCOMPARE(store.size(CaptureScope::Client, QStringLiteral("client")), std::size_t(4));
    QVERIFY(!store.contains(QStringLiteral("client-0")));
    // 两种作用域分别计额：连接作用域的 4 条仍保留。
    QCOMPARE(store.size(CaptureScope::Connection), std::size_t(4));
    QVERIFY(store.ownedByClient(QStringLiteral("client-4"), QStringLiteral("client")));
    QVERIFY(!store.ownedByClient(QStringLiteral("connection-4"), QStringLiteral("conn")));

    for (int i = 0; i < 32; ++i) {
        store.put(QStringLiteral("global-%1").arg(i),
            record(CaptureScope::Client, QStringLiteral("client-%1").arg(i),
                QStringLiteral("s1")));
    }
    // 每客户端 4 + 全局客户端作用域 32：总量由两个额度合成，不互相挤占连接作用域。
    QCOMPARE(store.size(CaptureScope::Client), std::size_t(32));
    QCOMPARE(store.size(CaptureScope::Connection), std::size_t(4));
}

void AuthorizationTests::capturesExpireAndFilterByOwner()
{
    CaptureStore store;
    CaptureRecord expired;
    expired.scope = CaptureScope::Client;
    expired.owner = QStringLiteral("a");
    expired.session = QStringLiteral("s1");
    expired.expires = 10;
    store.put(QStringLiteral("expired"), expired);
    CaptureRecord live = expired;
    live.owner = QStringLiteral("b");
    live.session = QStringLiteral("s2");
    live.expires = 100;
    store.put(QStringLiteral("live"), live);

    store.sweep(50);
    QVERIFY(!store.contains(QStringLiteral("expired")));
    QVERIFY(store.contains(QStringLiteral("live")));

    store.removeClientSession(QStringLiteral("b"), QStringLiteral("s2"));
    QCOMPARE(store.size(), std::size_t(0));

    CaptureRecord again = live;
    store.put(QStringLiteral("again"), again);
    store.removeClient(QStringLiteral("b"));
    QCOMPARE(store.size(), std::size_t(0));
}

void AuthorizationTests::snapshotStoreCoalescesWithinWindow()
{
    qint64 now = 0;
    SnapshotStore<int> store([&now] { return now; });
    int loads = 0;
    bool reused = false;
    const auto loader = [&loads, &now] {
        ++loads;
        return std::make_shared<const int>(int(now));
    };
    const auto first = store.acquire(QStringLiteral("s"), QStringLiteral("e"), loader, &reused);
    QVERIFY(first);
    QVERIFY(!reused);
    QCOMPARE(loads, 1);

    now = 100;
    const auto second = store.acquire(QStringLiteral("s"), QStringLiteral("e"), loader, &reused);
    QVERIFY(reused);
    QCOMPARE(loads, 1);
    QCOMPARE(*second, *first);

    now = 300;
    const auto third = store.acquire(QStringLiteral("s"), QStringLiteral("e"), loader, &reused);
    QVERIFY(!reused);
    QCOMPARE(loads, 2);
    QCOMPARE(*third, 300);

    const auto missing = store.acquire(QStringLiteral("missing"), QStringLiteral("e"),
        []() -> std::shared_ptr<const int> { return {}; }, &reused);
    QVERIFY(!missing);
    QVERIFY(!reused);
}

void AuthorizationTests::snapshotStoreReloadsOnEpochChange()
{
    qint64 now = 500;
    SnapshotStore<QString> store([&now] { return now; });
    int loads = 0;
    const auto loader = [&loads] {
        ++loads;
        return std::make_shared<const QString>(QStringLiteral("value-%1").arg(loads));
    };
    const auto first = store.acquire(QStringLiteral("s"), QStringLiteral("epoch-1"), loader);
    QCOMPARE(*first, QStringLiteral("value-1"));
    const auto rebound = store.acquire(QStringLiteral("s"), QStringLiteral("epoch-2"), loader);
    QCOMPARE(*rebound, QStringLiteral("value-2"));
    QCOMPARE(loads, 2);

    store.remove(QStringLiteral("s"));
    const auto reloaded = store.acquire(QStringLiteral("s"), QStringLiteral("epoch-2"), loader);
    QCOMPARE(*reloaded, QStringLiteral("value-3"));
}

void AuthorizationTests::captureSearchScopesAndLimits()
{
    CaptureRecord capture;
    capture.scope = CaptureScope::Client;
    capture.owner = QStringLiteral("client");
    capture.session = QStringLiteral("s1");
    capture.epoch = QStringLiteral("e1");
    capture.lines = QVector<CaptureLine>{
        CaptureLine{QStringLiteral("alpha beta alpha"), QStringLiteral("viewport"), 0},
        CaptureLine{QStringLiteral("Alpha"), QStringLiteral("recent_output"), 0}};

    const auto all = searchCapture(capture, QStringLiteral("alpha"), false,
        QStringLiteral("both"), 10);
    QCOMPARE(all.matches.size(), 3);
    QCOMPARE(all.matches[0].source, QStringLiteral("viewport"));
    QCOMPARE(all.matches[0].lineIndex, 0);
    QCOMPARE(all.matches[0].startByte, 0);
    QCOMPARE(all.matches[1].startByte, 11);
    QCOMPARE(all.matches[2].source, QStringLiteral("recent_output"));
    QVERIFY(!all.limited);

    const auto sensitive = searchCapture(capture, QStringLiteral("Alpha"), true,
        QStringLiteral("both"), 10);
    QCOMPARE(sensitive.matches.size(), 1);
    QCOMPARE(sensitive.matches[0].source, QStringLiteral("recent_output"));

    const auto viewport = searchCapture(capture, QStringLiteral("alpha"), false,
        QStringLiteral("viewport"), 10);
    QCOMPARE(viewport.matches.size(), 2);

    const auto limited = searchCapture(capture, QStringLiteral("alpha"), false,
        QStringLiteral("both"), 1);
    QCOMPARE(limited.matches.size(), 1);
    QVERIFY(limited.limited);

    const auto interrupted = searchCapture(capture, QStringLiteral("alpha"), false,
        QStringLiteral("both"), 10, [] { return true; });
    QVERIFY(interrupted.interrupted);
    QCOMPARE(interrupted.matches.size(), 0);
}

void AuthorizationTests::captureSearchFindsPatternPastChunkBoundary()
{
    CaptureRecord capture;
    capture.scope = CaptureScope::Connection;
    capture.owner = QStringLiteral("conn");
    capture.session = QStringLiteral("s1");
    capture.epoch = QStringLiteral("e1");
    capture.lines = QVector<CaptureLine>{CaptureLine{
        QString(9000, QLatin1Char('x')) + QStringLiteral("needle"),
        QStringLiteral("viewport"), 0}};
    const auto found = searchCapture(capture, QStringLiteral("needle"), true,
        QStringLiteral("both"), 10);
    QCOMPARE(found.matches.size(), 1);
    QCOMPARE(found.matches[0].startByte, 9000);
    QCOMPARE(found.matches[0].endByte, 9006);
}

void AuthorizationTests::confirmationsKeepNonceAndPendingState()
{
    ConfirmationRegistry registry;
    QVERIFY(registry.empty());

    registry.registerActive(QStringLiteral("conn-1"), QStringLiteral("client-1"),
        QStringLiteral("s1"), QStringLiteral("nonce-1"), 1000);
    QVERIFY(registry.isActive(QStringLiteral("conn-1"), QStringLiteral("nonce-1")));
    QVERIFY(registry.hasActiveForConnection(QStringLiteral("conn-1")));
    QVERIFY(!registry.isConsumed(QStringLiteral("nonce-1")));
    QVERIFY(!registry.empty());

    registry.consumeActive(QStringLiteral("conn-1"), QStringLiteral("nonce-1"), 1000);
    QVERIFY(!registry.isActive(QStringLiteral("conn-1"), QStringLiteral("nonce-1")));
    QVERIFY(registry.isConsumed(QStringLiteral("nonce-1")));

    registry.registerActive(QStringLiteral("conn-2"), QStringLiteral("client-2"),
        QStringLiteral("s2"), QStringLiteral("nonce-2"), 1000);
    QCOMPARE(registry.takeConnection(QStringLiteral("conn-2")), QStringLiteral("nonce-2"));
    QVERIFY(!registry.hasActiveForConnection(QStringLiteral("conn-2")));

    registry.registerActive(QStringLiteral("conn-3"), QStringLiteral("client-3"),
        QStringLiteral("s3"), QStringLiteral("nonce-3"), 1000);
    registry.registerActive(QStringLiteral("conn-4"), QStringLiteral("client-3"),
        QStringLiteral("s4"), QStringLiteral("nonce-4"), 1000);
    registry.removeScope(QStringLiteral("client-3"), QStringLiteral("s3"));
    QVERIFY(!registry.isActive(QStringLiteral("conn-3"), QStringLiteral("nonce-3")));
    QVERIFY(registry.isActive(QStringLiteral("conn-4"), QStringLiteral("nonce-4")));

    PendingConfirmationRecord pending;
    pending.jobKey = QStringLiteral("conn-1/7");
    pending.transaction.binding.connection = PeerId{QStringLiteral("conn-1")};
    pending.transaction.expires = 500;
    registry.insertPending(pending.jobKey, pending);
    QVERIFY(registry.hasPendingForConnection(QStringLiteral("conn-1")));
    QVERIFY(!registry.hasPendingForConnection(QStringLiteral("conn-4")));

    const auto live = registry.sweep(400);
    QVERIFY(live.empty());
    QVERIFY(registry.pendingCount() == 1);
    const auto expired = registry.sweep(600);
    QCOMPARE(expired.size(), std::size_t(1));
    QCOMPARE(expired[0].jobKey, QStringLiteral("conn-1/7"));
    QVERIFY(registry.pendingCount() == 0);
    QVERIFY(!registry.empty());
    // nonce-1 的 consumed 记录与 nonce-4 的 scope 均在到期后清理。
    static_cast<void>(registry.sweep(1000));
    QVERIFY(registry.empty());
}

void AuthorizationTests::requestSchedulerEnforcesBudgetsAndCancellation()
{
    RequestScheduler scheduler{RequestSchedulerLimits{2, 3}};
    QStringList delivered;
    const auto delivery = [&delivered](const QString& id) {
        return [&delivered, id] { delivered.append(id); };
    };
    using Admission = RequestScheduler::Admission;
    QCOMPARE(scheduler.enqueue(QStringLiteral("peer-1"), QStringLiteral("a"), delivery(QStringLiteral("a"))),
        Admission::Accepted);
    QCOMPARE(scheduler.enqueue(QStringLiteral("peer-1"), QStringLiteral("b"), delivery(QStringLiteral("b"))),
        Admission::Accepted);
    QCOMPARE(scheduler.enqueue(QStringLiteral("peer-1"), QStringLiteral("c"), delivery(QStringLiteral("c"))),
        Admission::Accepted);
    QCOMPARE(scheduler.enqueue(QStringLiteral("peer-1"), QStringLiteral("d"), delivery(QStringLiteral("d"))),
        Admission::QueueFull);
    QCOMPARE(scheduler.queuedCount(QStringLiteral("peer-1")), 3);

    auto ready = scheduler.takeReady();
    QCOMPARE(ready.size(), std::size_t(2));
    QCOMPARE(scheduler.activeCount(QStringLiteral("peer-1")), 2);
    QCOMPARE(scheduler.queuedCount(QStringLiteral("peer-1")), 1);
    for (auto& item : ready)
        item.run();
    QCOMPARE(delivered, QStringList({QStringLiteral("a"), QStringLiteral("b")}));

    // 另一 peer 的额度独立。
    QCOMPARE(scheduler.enqueue(QStringLiteral("peer-2"), QStringLiteral("x"), delivery(QStringLiteral("x"))),
        Admission::Accepted);
    QCOMPARE(scheduler.takeReady().size(), std::size_t(1));
    QVERIFY(scheduler.contains(QStringLiteral("peer-1"), QStringLiteral("c")));
    QVERIFY(scheduler.contains(QStringLiteral("peer-2"), QStringLiteral("x")));

    // 已排队请求取消后不会投递；完成一个活动请求后空出的额度可用。
    scheduler.cancel(QStringLiteral("peer-1"), QStringLiteral("c"));
    scheduler.complete(QStringLiteral("peer-1"), QStringLiteral("a"));
    QCOMPARE(scheduler.activeCount(QStringLiteral("peer-1")), 1);
    QCOMPARE(scheduler.enqueue(QStringLiteral("peer-1"), QStringLiteral("e"),
                 delivery(QStringLiteral("e"))), Admission::Accepted);
    ready = scheduler.takeReady();
    QCOMPARE(ready.size(), std::size_t(1));
    QCOMPARE(ready[0].request, QStringLiteral("e"));
    ready[0].run();
    QCOMPARE(delivered.last(), QStringLiteral("e"));
    QVERIFY(!scheduler.contains(QStringLiteral("peer-1"), QStringLiteral("c")));

    scheduler.removePeer(QStringLiteral("peer-1"));
    QCOMPARE(scheduler.peerCount(), std::size_t(1));
    QCOMPARE(scheduler.activeCount(QStringLiteral("peer-1")), 0);
}

void AuthorizationTests::commandDedupBlocksUntilSwept()
{
    CommandDedup dedup{std::chrono::milliseconds(5000)};
    QVERIFY(dedup.empty());
    QVERIFY(!dedup.blocked(QStringLiteral("target-a")));
    dedup.remember(QStringLiteral("target-a"), 1000);
    QVERIFY(dedup.blocked(QStringLiteral("target-a")));
    dedup.sweep(5999);
    QVERIFY(dedup.blocked(QStringLiteral("target-a")));
    dedup.sweep(6000);
    QVERIFY(!dedup.blocked(QStringLiteral("target-a")));
    QVERIFY(dedup.empty());
}


void AuthorizationTests::taskPhasesAdvanceOnlyForward()
{
    using NovaTerm::Mcp::TaskPhase;
    QVERIFY(NovaTerm::Mcp::taskPhaseCanAdvance(TaskPhase::Preparing, TaskPhase::Reserved));
    QVERIFY(NovaTerm::Mcp::taskPhaseCanAdvance(TaskPhase::Reserved, TaskPhase::Submitted));
    QVERIFY(NovaTerm::Mcp::taskPhaseCanAdvance(TaskPhase::Submitted, TaskPhase::Observing));
    QVERIFY(NovaTerm::Mcp::taskPhaseCanAdvance(TaskPhase::Observing, TaskPhase::Final));
    QVERIFY(NovaTerm::Mcp::taskPhaseCanAdvance(TaskPhase::Submitted, TaskPhase::Final));
    QVERIFY(!NovaTerm::Mcp::taskPhaseCanAdvance(TaskPhase::Observing, TaskPhase::Submitted));
    QVERIFY(!NovaTerm::Mcp::taskPhaseCanAdvance(TaskPhase::Final, TaskPhase::Final));
}

void AuthorizationTests::confirmationRejectsChangedFactsAndReplay()
{
    ConfirmationBinding current;
    current.connection = PeerId{QStringLiteral("peer")};
    current.client = ClientId{QStringLiteral("client")};
    current.session = SessionId{QStringLiteral("session")};
    current.epoch = Epoch{QStringLiteral("epoch")};
    current.targetFingerprint = QStringLiteral("target");
    current.policyVersion = QStringLiteral("policy");
    current.profileVersion = QStringLiteral("profile");
    current.grantVersion = ClientAccessVersion{7};
    current.promptGeneration = PromptRevision{8};
    current.userInputGeneration = InputRevision{9};
    current.payloadHash = QByteArray(32, 'h');
    const ConfirmationTransaction transaction{current, QStringLiteral("nonce"), 1000};
    QVERIFY(ConfirmationRegistry::validate(transaction, current, 999));
    QVERIFY(!ConfirmationRegistry::validate(transaction, current, 1000));
    const std::vector<std::function<void(ConfirmationBinding&)>> changeFact{
        [](auto& value) { value.connection = PeerId{QStringLiteral("other")}; },
        [](auto& value) { value.client = ClientId{QStringLiteral("other")}; },
        [](auto& value) { value.session = SessionId{QStringLiteral("other")}; },
        [](auto& value) { value.epoch = Epoch{QStringLiteral("other")}; },
        [](auto& value) { value.targetFingerprint += QLatin1Char('x'); },
        [](auto& value) { value.policyVersion += QLatin1Char('x'); },
        [](auto& value) { value.profileVersion += QLatin1Char('x'); },
        [](auto& value) { value.grantVersion = ClientAccessVersion{10}; },
        [](auto& value) { value.promptGeneration = PromptRevision{10}; },
        [](auto& value) { value.userInputGeneration = InputRevision{10}; },
        [](auto& value) { value.payloadHash[0] = 'x'; },
        [](auto& value) { value.tool = ConfirmationTool::Script; },
        [](auto& value) { value.decision = ConfirmationDecision::Unknown; },
    };
    ConfirmationRegistry registry;
    registry.registerActive(transaction);
    for (const auto& change : changeFact) {
        auto stale = current;
        change(stale);
        QVERIFY(!ConfirmationRegistry::validate(transaction, stale, 999));
        QVERIFY(!registry.consume(transaction, stale, 999));
    }
    auto alteredDeadline = transaction;
    alteredDeadline.expires += 1000;
    QVERIFY(!registry.consume(alteredDeadline, current, 999));
    QVERIFY(!registry.consume(transaction, current, 1000));
    QVERIFY(registry.consume(transaction, current, 999));
    QVERIFY(!registry.consume(transaction, current, 999));
    registry.registerActive(transaction);
    QVERIFY(!registry.consume(transaction, current, 999));
    static_cast<void>(registry.sweep(1000));
    QVERIFY(!registry.consume(transaction, current, 1001));
}

void AuthorizationTests::confirmationUserGenerationPreservesPresence()
{
    for (auto tool : {ConfirmationTool::Command, ConfirmationTool::Script}) {
        ConfirmationBinding current;
        current.tool = tool;
        ConfirmationTransaction transaction{current, QStringLiteral("nonce"), 1000};
        QVERIFY(ConfirmationRegistry::validate(transaction, current, 1));
        current.userInputGeneration = InputRevision{0};
        QVERIFY(!ConfirmationRegistry::validate(transaction, current, 1));
        transaction.binding.userInputGeneration = InputRevision{0};
        QVERIFY(ConfirmationRegistry::validate(transaction, current, 1));
        current.userInputGeneration = InputRevision{1};
        QVERIFY(!ConfirmationRegistry::validate(transaction, current, 1));
    }
}

QTEST_MAIN(AuthorizationTests)
#include "AuthorizationTests.moc"
