#pragma once
#include "CapabilityDispatcher.h"
#include "SessionExecutionRouter.h"
#include <QByteArray>
namespace NovaTerm::Capabilities {
struct RecipeStep {
    QByteArray command;
    qsizetype maxOutputBytes{65536};
    std::chrono::milliseconds timeout{5000};
    QString commandId{};
};
struct RecipeInvocation { RecipeRequirements recipe; std::vector<RecipeStep> steps; };
struct RecipeStepResult {
    bool success{false}, executionMayHaveStarted{false}, terminationConfirmed{false};
    QByteArray output;
};
using StepCompletion = std::function<void(RecipeStepResult)>;
enum class DiagnosticOutcome { Completed, Failed, Denied, Busy, Cancelled, TimedOut, Unknown };
struct DiagnosticResult {
    DiagnosticOutcome outcome{DiagnosticOutcome::Failed};
    bool executionMayHaveStarted{false}, terminationConfirmed{false}, outputTruncated{false};
    QByteArray output;
};
class RecipeSessionPort
{
public:
    virtual ~RecipeSessionPort() = default;
    virtual TargetFacts facts(const SessionBinding&) = 0;
    virtual bool diagnosticAuthorized(const InvocationContext&, const CapabilityId&) = 0;
    virtual bool sharedOccupancyConsented(const InvocationContext&, const RecipeRequirements&, quint64) { return false; }
    virtual bool reserve(const SessionBinding&, const OperationId&) = 0;
    virtual bool consumeReadiness(const SessionBinding&, quint64 revision, const QString& evidence) = 0;
    // False guarantees no submission. There is no automatic fallback or retry.
    // Completion is delivered on the owning application thread.
    virtual bool submit(ExecutionRoute, const SessionBinding&, const OperationId&, const RecipeStep&, StepCompletion) = 0;
    virtual void cancel(const SessionBinding&, const OperationId&) = 0;
    // false MUST retain the target reservation/quarantine, including on destruction.
    virtual void settle(const SessionBinding&, const OperationId&, bool terminationConfirmed) = 0;
};
class DiagnosticTaskManager
{
public:
    explicit DiagnosticTaskManager(RecipeSessionPort&, std::function<TimePoint()> now = Clock::now);
    ~DiagnosticTaskManager();
    DiagnosticTaskManager(const DiagnosticTaskManager&) = delete;
    DiagnosticTaskManager& operator=(const DiagnosticTaskManager&) = delete;
    void start(RecipeInvocation, InvocationLease, std::function<void(DiagnosticResult)>);
    bool cancel(const PrincipalId&, const OperationId&);
    void sweep();
    void stop();
    size_t activeCount() const;
    static constexpr size_t MaxTasks = 32, MaxPrincipalTasks = 4, MaxSteps = 64;
    static constexpr qsizetype MaxOutputBytes = 65536;
private:
    class Impl;
    std::shared_ptr<Impl> _impl;
};
}
