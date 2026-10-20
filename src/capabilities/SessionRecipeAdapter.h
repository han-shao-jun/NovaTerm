#pragma once
#include "DiagnosticTaskManager.h"
class SessionDirectory;
namespace NovaTerm::Capabilities {
// Session integration only. Permission is an explicit Recipe grant supplied by the
// composition root; FullTerminalInput is never consulted or promoted here.
class SessionRecipeAdapter final : public RecipeSessionPort
{
public:
    using OccupancyConsent = std::function<bool(const InvocationContext&, const RecipeRequirements&, quint64)>;
    using Permission = std::function<bool(const InvocationContext&, const CapabilityId&)>;
    SessionRecipeAdapter(SessionDirectory&, QString targetDirectory, QString instance, Permission, OccupancyConsent = {});
    ~SessionRecipeAdapter() override;
    SessionRecipeAdapter(const SessionRecipeAdapter&) = delete;
    SessionRecipeAdapter& operator=(const SessionRecipeAdapter&) = delete;
    TargetFacts facts(const SessionBinding&) override;
    bool diagnosticAuthorized(const InvocationContext&, const CapabilityId&) override;
    bool sharedOccupancyConsented(const InvocationContext&, const RecipeRequirements&, quint64) override;
    bool reserve(const SessionBinding&, const OperationId&) override;
    bool consumeReadiness(const SessionBinding&, quint64, const QString&) override;
    bool submit(ExecutionRoute, const SessionBinding&, const OperationId&, const RecipeStep&, StepCompletion) override;
    void cancel(const SessionBinding&, const OperationId&) override;
    void settle(const SessionBinding&, const OperationId&, bool) override;
private:
    class Impl;
    std::shared_ptr<Impl> _impl;
};
}
