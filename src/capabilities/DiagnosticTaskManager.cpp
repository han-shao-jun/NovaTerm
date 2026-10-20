#include "DiagnosticTaskManager.h"
#include <map>
namespace NovaTerm::Capabilities {
class DiagnosticTaskManager::Impl : public std::enable_shared_from_this<DiagnosticTaskManager::Impl>
{
public:
    using Key = std::pair<PrincipalId, OperationId>;
    struct Job {
        RecipeInvocation invocation;
        InvocationLease lease;
        SessionBinding binding;
        std::function<void(DiagnosticResult)> done;
        DiagnosticResult result;
        size_t step{0};
        quint64 revision{0};
        TimePoint stepDeadline;
        bool reserved{false}, inFlight{false};
    };
    Impl(RecipeSessionPort& session, std::function<TimePoint()> now)
        : session(session), now(std::move(now)) {}
    void close() {
        closed = stopping = true;
        auto owned = std::move(jobs);
        // Destruction drops client delivery but leaves unknown targets protected.
        for (const auto& item : owned) {
            const auto& job = *item.second;
            if (job.inFlight) session.cancel(job.binding, item.first.second);
            if (job.reserved) session.settle(job.binding, item.first.second, !job.inFlight);
        }
    }
    void finish(const Key& key, DiagnosticOutcome outcome, bool terminated, bool cancelRemote = false)
    {
        auto node = jobs.extract(key);
        if (node.empty()) return;
        auto job = std::move(node.mapped());
        if (cancelRemote) session.cancel(job->binding, key.second);
        if (job->reserved) session.settle(job->binding, key.second, terminated);
        job->result.outcome = outcome;
        job->result.terminationConfirmed = terminated;
        // Revocation must suppress already collected body, even on cancellation/error.
        const auto denied = job->lease.checkAuthorization();
        if (closed) return;
        const bool permitted = !denied && session.diagnosticAuthorized(job->lease.context(), job->invocation.recipe.id);
        if (closed) return;
        if (!permitted) {
            job->result.output.clear();
            job->result.outcome = DiagnosticOutcome::Denied;
        }
        auto done = std::move(job->done);
        done(std::move(job->result));
        job->lease.finish();
    }
    bool current(const Key& key, const std::shared_ptr<Job>& job) const {
        const auto it = jobs.find(key);
        return !closed && it != jobs.end() && it->second == job;
    }
    void run(const Key& key)
    {
        const auto it = jobs.find(key);
        if (it == jobs.end()) return;
        const auto keep = it->second;
        auto& job = *keep;
        const auto denied = job.lease.check();
        if (!current(key, keep)) return;
        const bool permitted = !denied && session.diagnosticAuthorized(job.lease.context(), job.invocation.recipe.id);
        if (!current(key, keep)) return;
        if (!permitted) {
            finish(key, DiagnosticOutcome::Denied, true); return;
        }
        const auto facts = session.facts(job.binding);
        if (!current(key, keep)) return;
        if (!sameResource(job.binding, facts.environment.binding)
            || (job.revision && job.revision != facts.environment.revision)) {
            finish(key, DiagnosticOutcome::Denied, true); return;
        }
        const bool consent = facts.environment.source == EnvironmentSource::PairedAdapter
            && session.sharedOccupancyConsented(job.lease.context(), job.invocation.recipe, facts.environment.revision);
        if (!current(key, keep)) return;
        const auto route = SessionExecutionRouter::resolve(facts, job.invocation.recipe,
            RecipeAuthorization{key.first, true, false, consent}, now());
        if (!route.route) { finish(key, DiagnosticOutcome::Denied, true); return; }
        job.revision = route.environmentRevision;
        if (!job.reserved) {
            const bool reserved = session.reserve(job.binding, key.second);
            if (!current(key, keep)) return;
            if (!reserved) { finish(key, DiagnosticOutcome::Busy, true); return; }
            job.reserved = true;
        }
        if (*route.route == ExecutionRoute::SharedTerminal) {
            const bool ready = session.consumeReadiness(job.binding, route.environmentRevision, route.readinessEvidenceId);
            if (!current(key, keep)) return;
            if (!ready) { finish(key, DiagnosticOutcome::Denied, true); return; }
        }
        // Final submit check; reserve/consent callbacks may have revoked authority.
        const auto finalDenied = job.lease.check();
        if (!current(key, keep)) return;
        const bool finalPermission = !finalDenied && session.diagnosticAuthorized(job.lease.context(), job.invocation.recipe.id);
        if (!current(key, keep)) return;
        if (!finalPermission) { finish(key, DiagnosticOutcome::Denied, true); return; }
        const auto stepIndex = job.step;
        const auto step = job.invocation.steps[stepIndex];
        const auto binding = job.binding;
        job.inFlight = true;
        job.stepDeadline = now() + step.timeout;
        const std::weak_ptr<Impl> weak = weak_from_this();
        const std::weak_ptr<Job> expected = keep;
        const auto accepted = session.submit(*route.route, binding, key.second, step,
            [weak, expected, key, stepIndex](RecipeStepResult result) {
                const auto job = expected.lock();
                if (const auto owner = weak.lock(); owner && job && owner->current(key, job))
                    owner->completed(key, stepIndex, std::move(result));
            });
        // Synchronous completion may have removed this job.
        if (!current(key, keep)) return;
        if (!accepted && keep->step == stepIndex)
            finish(key, DiagnosticOutcome::Failed, true);
    }
    void completed(const Key& key, size_t step, RecipeStepResult result)
    {
        const auto it = jobs.find(key);
        if (it == jobs.end() || it->second->step != step || !it->second->inFlight) return;
        const auto keep = it->second;
        auto& job = *keep;
        job.inFlight = false;
        job.result.executionMayHaveStarted |= result.executionMayHaveStarted;
        const auto room = (std::min)(MaxOutputBytes - job.result.output.size(),
            job.invocation.steps[step].maxOutputBytes);
        job.result.outputTruncated |= result.output.size() > room;
        job.result.output.append(result.output.constData(), (std::min)(room, result.output.size()));
        if (!result.terminationConfirmed) { finish(key, DiagnosticOutcome::Unknown, false); return; }
        const auto denied = job.lease.check();
        if (!current(key, keep)) return;
        const bool allowed = !denied && session.diagnosticAuthorized(job.lease.context(), job.invocation.recipe.id);
        if (!current(key, keep)) return;
        if (!allowed) { job.result.output.clear(); finish(key, DiagnosticOutcome::Denied, true); return; }
        if (!result.success) { finish(key, DiagnosticOutcome::Failed, true); return; }
        if (++job.step == job.invocation.steps.size()) { finish(key, DiagnosticOutcome::Completed, true); return; }
        run(key);
    }
    bool cancel(const Key& key, DiagnosticOutcome reason)
    {
        const auto it = jobs.find(key);
        if (it == jobs.end()) return false;
        const bool inFlight = it->second->inFlight;
        if (inFlight) it->second->result.executionMayHaveStarted = true;
        // Complete locally first; synchronous cancel results cannot emit a second reply.
        finish(key, inFlight ? DiagnosticOutcome::Unknown : reason, !inFlight, inFlight);
        return true;
    }
    RecipeSessionPort& session;
    std::function<TimePoint()> now;
    std::map<Key, std::shared_ptr<Job>> jobs;
    bool stopping{false}, closed{false};
};
DiagnosticTaskManager::DiagnosticTaskManager(RecipeSessionPort& port, std::function<TimePoint()> now)
    : _impl(std::make_shared<Impl>(port, std::move(now))) {}
DiagnosticTaskManager::~DiagnosticTaskManager() { _impl->close(); }
void DiagnosticTaskManager::start(RecipeInvocation invocation, InvocationLease lease,
    std::function<void(DiagnosticResult)> done)
{
    const auto impl = _impl; // Keep an executing stack alive if delivery destroys the facade.
    const auto reject = [&lease, &done](DiagnosticOutcome outcome) {
        done(DiagnosticResult{outcome, false, true, false, {}}); lease.finish();
    };
    if (lease.check() || impl->stopping) { reject(DiagnosticOutcome::Denied); return; }
    const auto context = lease.context();
    const auto* binding = std::get_if<SessionBinding>(&context.resource);
    if (lease.capability() != invocation.recipe.id || !binding || invocation.steps.empty() || invocation.steps.size() > MaxSteps) { reject(DiagnosticOutcome::Denied); return; }
    for (const auto& step : invocation.steps) {
        if (step.command.isEmpty() || step.command.size() > 16384 || step.maxOutputBytes < 1
            || step.maxOutputBytes > MaxOutputBytes || step.timeout <= std::chrono::milliseconds(0)
            || step.timeout > std::chrono::seconds(30)) { reject(DiagnosticOutcome::Denied); return; }
    }
    size_t principalTasks = 0;
    for (const auto& item : impl->jobs) if (item.first.first == context.principal) ++principalTasks;
    if (impl->jobs.size() >= MaxTasks || principalTasks >= MaxPrincipalTasks) { reject(DiagnosticOutcome::Busy); return; }
    const Impl::Key key{context.principal, context.operation};
    if (impl->jobs.count(key)) { reject(DiagnosticOutcome::Busy); return; }
    auto job = std::make_shared<Impl::Job>();
    job->binding = *binding;
    job->invocation = std::move(invocation);
    job->lease = std::move(lease);
    job->done = std::move(done);
    impl->jobs.emplace(key, std::move(job));
    const std::weak_ptr<Impl> weak = impl;
    impl->jobs.at(key)->lease.onCancel([weak, key] {
        if (const auto owner = weak.lock(); owner && !owner->closed)
            owner->cancel(key, DiagnosticOutcome::Cancelled);
    });
    impl->run(key);
}
bool DiagnosticTaskManager::cancel(const PrincipalId& principal, const OperationId& operation)
{ const auto impl = _impl; return impl->cancel({principal, operation}, DiagnosticOutcome::Cancelled); }
void DiagnosticTaskManager::sweep()
{
    const auto impl = _impl;
    // Authorization/clock callbacks may cancel jobs or destroy the facade.
    std::vector<std::pair<Impl::Key, std::shared_ptr<Impl::Job>>> pending;
    pending.reserve(impl->jobs.size());
    for (const auto& item : impl->jobs) pending.emplace_back(item.first, item.second);
    for (const auto& item : pending) {
        if (!impl->current(item.first, item.second)) continue;
        const auto denied = item.second->lease.check();
        if (!impl->current(item.first, item.second)) continue;
        const auto now = impl->now();
        if (!impl->current(item.first, item.second)) continue;
        if (denied || (now >= item.second->stepDeadline && item.second->inFlight))
            impl->cancel(item.first, DiagnosticOutcome::TimedOut);
    }
}
void DiagnosticTaskManager::stop()
{
    const auto impl = _impl;
    impl->stopping = true;
    std::vector<Impl::Key> keys;
    for (const auto& item : impl->jobs) keys.push_back(item.first);
    for (const auto& key : keys) { if (impl->closed) break; impl->cancel(key, DiagnosticOutcome::Cancelled); }
}
size_t DiagnosticTaskManager::activeCount() const { return _impl->jobs.size(); }
}
