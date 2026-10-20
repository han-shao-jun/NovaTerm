#include "SessionRecipeAdapter.h"
#include "session/SessionDirectory.h"
#include "session/SessionCommandFacade.h"
#include "session/SessionCommandCoordinator.h"
#include "session/SessionInputArbiter.h"
#include "mcp/CommandPolicy.h"
#include "core/terminal/TerminalCore.h"
#include <QObject>
#include <QUuid>
#include <map>
namespace NovaTerm::Capabilities {
namespace {
SessionBinding bindingOf(const SessionDirectory::Entry& entry)
{
    SessionBinding binding{SessionId{entry.id}, AttachmentId{entry.attachmentId}, Epoch{entry.epoch}, entry.generation, {}};
    if (!entry.targetFingerprint.isEmpty()) binding.targetFingerprint = entry.targetFingerprint;
    return binding;
}
TargetTransport transportOf(TransportKind kind)
{
    switch (kind) {
    case TransportKind::LocalShell: return TargetTransport::Local;
    case TransportKind::Ssh: return TargetTransport::Ssh;
    case TransportKind::Serial: return TargetTransport::Serial;
    case TransportKind::Telnet: return TargetTransport::Telnet;
    case TransportKind::Custom: return TargetTransport::Custom;
    }
    return TargetTransport::Custom;
}
}
class SessionRecipeAdapter::Impl
{
public:
    struct Submission {
        SessionBinding binding;
        quint64 request{0};
        ExecutionRoute route{ExecutionRoute::Isolated};
        StepCompletion done;
        QMetaObject::Connection finished;
    };
    Impl(SessionDirectory& directory, QString path, QString instance, Permission permission, OccupancyConsent consent)
        : directory(directory), guard(std::move(path)), instance(std::move(instance)), permission(std::move(permission)), consent(std::move(consent)) {}
    std::optional<SessionDirectory::Entry> entry(const SessionBinding& binding)
    {
        const auto current = directory.find(binding.session.value());
        if (!current || !current->session || !sameResource(bindingOf(*current), binding)) return std::nullopt;
        return current;
    }
    SessionDirectory& directory;
    Mcp::TargetGuard guard;
    QString instance;
    Permission permission;
    OccupancyConsent consent;
    QObject receiver;
    quint64 nextRequest{quint64(1) << 62}; // Session facade/coordinator also serve legacy callers.
    std::map<OperationId, Submission> active;
    bool closed{false};
    void close() {
        closed = true;
        auto owned = std::move(active);
        for (const auto& item : owned) {
            QObject::disconnect(item.second.finished);
            const auto current = entry(item.second.binding);
            if (!current) continue;
            if (item.second.route == ExecutionRoute::Isolated) current->session->commandFacade()->cancel(item.second.request);
            else current->session->commandCoordinator()->cancel(item.second.request);
        }
    }
};
SessionRecipeAdapter::SessionRecipeAdapter(SessionDirectory& directory, QString path, QString instance, Permission permission, OccupancyConsent consent)
    : _impl(std::make_shared<Impl>(directory, std::move(path), std::move(instance), std::move(permission), std::move(consent))) {}
SessionRecipeAdapter::~SessionRecipeAdapter() { _impl->close(); }
TargetFacts SessionRecipeAdapter::facts(const SessionBinding& binding)
{
    TargetFacts facts;
    const auto entry = _impl->entry(binding);
    if (!entry) return facts;
    auto* session = entry->session.data();
    facts.transport = transportOf(entry->kind);
    facts.environment = session->environmentTracker().snapshot();
    const bool running = entry->state == SessionState::Running && entry->transport && entry->transport->isConnected();
    auto* facade = session->commandFacade();
    facts.isolatedAvailable = running && facade->isAvailable()
        && facade->capabilities().mode == CommandExecutionMode::Isolated;
    facts.sharedAvailable = running && session->commandCoordinator();
    facts.fileTransfer = session->inputArbiter()->hasTransferLease();
    facts.occupied = session->inputArbiter()->hasMcpLease();
    const auto context = session->tryTerminalContext();
    facts.tuiOrReplOrLogin = context && context->state.alternateScreen;
    return facts;
}
bool SessionRecipeAdapter::diagnosticAuthorized(const InvocationContext& context, const CapabilityId& recipe)
{ return _impl->permission && _impl->permission(context, recipe); }
bool SessionRecipeAdapter::sharedOccupancyConsented(const InvocationContext& context, const RecipeRequirements& recipe, quint64 revision)
{ const auto impl = _impl; return impl->consent && impl->consent(context, recipe, revision); }
bool SessionRecipeAdapter::reserve(const SessionBinding& binding, const OperationId& operation)
{
    const auto entry = _impl->entry(binding);
    if (!entry || !binding.targetFingerprint || binding.targetFingerprint->isEmpty()) return false;
    QString error;
    return _impl->guard.reserve(*binding.targetFingerprint, operation.value(), _impl->instance, error);
}
bool SessionRecipeAdapter::consumeReadiness(const SessionBinding& binding, quint64 revision, const QString& evidence)
{
    const auto entry = _impl->entry(binding);
    return entry && entry->session->environmentTracker().consumeReadiness(revision, evidence);
}
bool SessionRecipeAdapter::submit(ExecutionRoute route, const SessionBinding& binding,
    const OperationId& operation, const RecipeStep& step, StepCompletion done)
{
    const auto impl = _impl;
    const auto entry = impl->entry(binding);
    if (!entry || impl->closed || impl->active.count(operation) || impl->active.size() >= DiagnosticTaskManager::MaxTasks) return false;
    const auto target = facts(binding);
    if ((route == ExecutionRoute::Isolated && !target.isolatedAvailable)
        || (route == ExecutionRoute::SharedTerminal && (!target.sharedAvailable || target.occupied
            || target.fileTransfer || target.tuiOrReplOrLogin))) return false;
    CommandExecutionRequest request;
    request.requestId = ++impl->nextRequest;
    request.command = step.command;
    request.commandId = step.commandId;
    request.limits.maxOutputBytes = step.maxOutputBytes;
    request.limits.timeoutMs = int(step.timeout.count());
    auto* session = entry->session.data();
    auto* coordinator = session->commandCoordinator();
    const bool promptFree = coordinator->allowsUnverifiedPrompt() || coordinator->allowsIdleCompletion();
    request.expectedPromptGeneration = promptFree ? 0 : coordinator->promptGeneration();
    if (route == ExecutionRoute::SharedTerminal)
        request.executionNonce = QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8();
    Impl::Submission submission{binding, request.requestId, route, std::move(done), {}};
    const std::weak_ptr<Impl> weak = impl;
    const auto finished = [weak, operation, id = request.requestId](const CommandExecutionResult& result) {
        const auto owner = weak.lock();
        if (!owner || owner->closed || result.requestId != id) return;
        auto node = owner->active.extract(operation);
        if (node.empty()) return;
        auto submission = std::move(node.mapped());
        QObject::disconnect(submission.finished);
        const auto stillBound = owner->entry(submission.binding).has_value();
        submission.done({result.outcome == CommandExecutionOutcome::Completed && stillBound,
            result.executionMayHaveStarted, result.terminationConfirmed && stillBound,
            result.standardOutput + result.standardError});
    };
    submission.finished = route == ExecutionRoute::Isolated
        ? QObject::connect(session->commandFacade(), &SessionCommandFacade::finished, &impl->receiver, finished)
        : QObject::connect(session->commandCoordinator(), &SessionCommandCoordinator::finished, &impl->receiver, finished);
    impl->active.emplace(operation, std::move(submission));
    const bool accepted = route == ExecutionRoute::Isolated
        ? session->commandFacade()->execute(request) : session->commandCoordinator()->submit(request);
    if (impl->closed) return accepted;
    if (!accepted) {
        auto node = impl->active.extract(operation);
        if (!node.empty()) QObject::disconnect(node.mapped().finished);
    }
    return accepted;
}
void SessionRecipeAdapter::cancel(const SessionBinding& binding, const OperationId& operation)
{
    const auto impl = _impl;
    auto node = impl->active.extract(operation);
    if (node.empty()) return;
    auto owned = std::move(node.mapped());
    QObject::disconnect(owned.finished);
    const auto entry = impl->entry(binding);
    if (entry) {
        if (owned.route == ExecutionRoute::Isolated) entry->session->commandFacade()->cancel(owned.request);
        else entry->session->commandCoordinator()->cancel(owned.request);
    }
    // Drop the bounded callback slot; unknown target protection remains on disk.
    owned.done({false, true, false, {}});
}
void SessionRecipeAdapter::settle(const SessionBinding& binding, const OperationId& operation, bool terminated)
{
    if (terminated && binding.targetFingerprint)
        _impl->guard.release(*binding.targetFingerprint, operation.value());
}
}
