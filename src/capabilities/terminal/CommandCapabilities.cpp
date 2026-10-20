#include "CommandCapabilities.h"
#include "mcp/contracts/Budget.h"
#include "capabilities/terminal/LegacyTerminalMapping.h"
namespace NovaTerm::Capabilities::Terminal {
CommandCapabilities::CommandCapabilities(CapabilityDispatcher& dispatcher, Mcp::CommandRuntime& runtime)
    : diagnostic(dispatcher, CapabilityId{"terminal.diagnostic.execute"},
        [this](auto request, InvocationLease lease, auto done) {
            start(std::move(request), CapabilityId{"terminal.diagnostic.execute"}, std::move(lease), std::move(done));
        })
    , command(dispatcher, CapabilityId{"terminal.command.run"},
        [this](auto request, InvocationLease lease, auto done) {
            start(std::move(request), CapabilityId{"terminal.command.run"}, std::move(lease), std::move(done));
        })
    , script(dispatcher, CapabilityId{"terminal.script.run"},
        [this](auto request, InvocationLease lease, auto done) {
            start(std::move(request), CapabilityId{"terminal.script.run"}, std::move(lease), std::move(done));
        })
    , _runtime(runtime)
{}
CommandCapabilities::~CommandCapabilities() { stop(); }
void CommandCapabilities::start(Mcp::CommandInvocation invocation, const CapabilityId& capability,
    InvocationLease lease, Completion<Response> done)
{
    const auto& context = lease.context();
    const auto* binding = std::get_if<SessionBinding>(&context.resource);
    const auto reference = Mcp::sessionReference(invocation.request);
    if (Mcp::terminalCapability(invocation.request) != capability || !binding || !reference
        || binding->session.value() != reference->session.str() || binding->epoch.value() != reference->epoch.str()) {
        done(Rejection::ResourceMismatch); return;
    }
    const auto boundedText = [](const QString& text, qsizetype max) {
        return !text.isEmpty() && text.size() <= max && text.toUtf8().size() <= max && !text.contains(QChar::Null);
    };
    const bool valid = std::visit([&boundedText](const auto& request) {
        using T = std::decay_t<decltype(request)>;
        if constexpr (std::is_same_v<T, Mcp::RunCommandRequest>)
            return boundedText(request.command, 16384) && request.timeout.count() > 0 && request.timeout.count() <= 30000;
        else if constexpr (std::is_same_v<T, Mcp::RunScriptRequest>)
            return boundedText(request.scriptContent, Mcp::Budget::MaxScriptBytes) && boundedText(request.targetPath, 4096)
                && boundedText(request.workingDirectory, 4096) && boundedText(request.invocation, 16384)
                && request.timeout.count() > 0 && request.timeout.count() <= 30000;
        else if constexpr (std::is_same_v<T, Mcp::ExecuteCommandRequest>) return !request.hasDiagnosticArguments;
        else return false;
    }, invocation.request);
    if (!valid) { Mcp::CommandResult result; result.error = Mcp::Error{Mcp::ErrorCode::InvalidArgument}; done(Response{result}); return; }
    if (invocation.task.isEmpty() || _active.count(invocation.task.str())) { done(Rejection::DuplicateOperation); return; }
    invocation.client = Mcp::ClientId{context.principal.value()};
    invocation.admission = [validate = lease.validation()]() -> std::optional<Mcp::Error> {
        if (const auto reason = validate()) return Mcp::Error{
            *reason == Rejection::DeadlineExceeded ? Mcp::ErrorCode::DeadlineExceeded : Mcp::ErrorCode::SessionNotAvailable};
        return std::nullopt;
    };
    const auto task = invocation.task;
    lease.onCancel([this, task] { cancel(task); });
    _active.emplace(task.str(), Active{std::move(lease), std::move(done), invocation.connection});
    // No second submission path: the original CommandRuntime owns the whole lifecycle.
    _runtime.start(std::move(invocation));
}
void CommandCapabilities::complete(const Mcp::TaskId& task, Response result)
{
    auto node = _active.extract(task.str());
    if (node.empty()) return;
    auto active = std::move(node.mapped());
    active.done(std::move(result));
}
void CommandCapabilities::cancel(const Mcp::TaskId& task)
{
    auto node = _active.extract(task.str());
    if (!node.empty()) node.mapped().lease.finish();
    // Local delivery is retired even when the runtime must retain remote uncertainty.
    _runtime.cancel(task);
}
void CommandCapabilities::disconnect(const QString& peer)
{
    std::vector<Mcp::TaskId> tasks;
    for (const auto& item : _active) if (item.second.peer.str() == peer) tasks.emplace_back(item.first);
    for (const auto& task : tasks) cancel(task);
    _runtime.disconnect(peer);
}
void CommandCapabilities::stop()
{
    auto owned = std::move(_active);
    for (auto& item : owned) item.second.lease.finish();
    for (const auto& item : owned) _runtime.cancel(Mcp::TaskId{item.first});
    _runtime.stop();
}
void CommandCapabilities::deadline(const Mcp::TaskId& task, TimePoint deadline)
{
    const auto it = _active.find(task.str());
    if (it != _active.end()) it->second.lease.setDeadline(deadline);
}
}
