/** @file CommandRuntime.h @brief Typed command lifecycle, independent of Session instances and protocols. */
#pragma once
#include "mcp/contracts/Errors.h"
#include "mcp/contracts/TaskPhases.h"
#include "mcp/contracts/ToolRequests.h"
#include "session/CommandExecutionTypes.h"
#include <QHash>
#include <QList>
#include <QSet>
#include <QStringList>
#include <functional>
#include <memory>

namespace NovaTerm::Mcp {
enum class CommandSessionKind { Local, Ssh, Serial, Other };

// Immutable authorization/readiness facts. Channel IDs are resolved only by the Session port.
struct CommandSessionFacts
{
    SessionReference reference;
    CommandSessionKind kind{CommandSessionKind::Other};
    QString target, profileVersion, diagnosticPolicyVersion;
    quint64 sessionGeneration{0}, grantVersion{0}, promptGeneration{0}, userInputGeneration{0};
    quint64 executor{0}, coordinator{0}, provider{0};
    bool running{false}, readable{false}, commandAllowed{false}, scriptAllowed{false};
    bool coordinatorAvailable{false}, trustedProfile{false}, promptReady{false}, promptFree{false};
    bool executorAvailable{false}, profileAvailable{false}, providerAvailable{false};
    CommandExecutionMode mode{CommandExecutionMode::Isolated};
    QHash<QString, QByteArray> diagnostics;
    QSet<QString> allowedDiagnostics;
};
struct PreparedScript { QString targetPath, command; bool targetExistsLocally{false}; };
struct ScriptPublicationRequest
{
    quint64 requestId{0};
    QByteArray content;
    QString targetPath, workingDirectory;
};
struct ScriptPublicationResult
{
    quint64 requestId{0};
    bool success{false};
    std::optional<ErrorCode> error;
    QString resolvedTargetPath;
    QByteArray contentHash;
};

// Actual Session and authorization boundary; no Session/Provider pointers escape it.
class CommandSessionPort
{
public:
    virtual ~CommandSessionPort() = default;
    virtual std::optional<CommandSessionFacts> session(const ClientId&, const SessionId&) = 0;
    virtual Result<PreparedScript> prepareScript(const CommandSessionFacts&, const RunScriptRequest&) = 0;
    virtual bool submit(quint64 channel, bool coordinator, const CommandExecutionRequest&) = 0;
    virtual void cancel(quint64 channel, bool coordinator, quint64 request, bool expired) = 0;
    virtual bool publish(quint64 provider, const ScriptPublicationRequest&) = 0;
    virtual void cancelPublication(quint64 provider, quint64 request) = 0;
};
struct TargetProtection { QString target, execution, instance, createdAt; };
class CommandTargetStore
{
public:
    virtual ~CommandTargetStore() = default;
    virtual Result<std::monostate> reserve(const TargetProtection&) = 0;
    virtual bool release(const QString& target, const QString& execution) = 0;
    virtual QList<TargetProtection> protections() const = 0;
    virtual bool acknowledge(const QString& target) = 0;
};
struct DiagnosticTicket
{
    QString execution;
    SessionReference session;
    DiagnosticId command;
    QString policyVersion, profileVersion;
    ClientAccessVersion grantVersion;
    qint64 expires{0};
};
// Decoded call. Signatures, JSON and bearer keys belong to the integration adapter.
struct CommandInvocation
{
    TaskId task;
    PeerId connection;
    ClientId client;
    ToolRequest request;
    QByteArray confirmationDigest;
    std::optional<DiagnosticTicket> ticket;
    std::optional<ConfirmationTransaction> confirmation;
    std::optional<ConfirmationAnswer> answer;
    bool confirmationSupplied{false}, detachedConfirmation{false}, formElicitation{false};
    // Optional application admission boundary. Checked at every deferred submission.
    std::function<std::optional<Error>()> admission;
};
struct CommandExecutionSummary
{
    QString executionId, instanceId, commandId, policyVersion;
    SessionReference session;
    QString status, finishedAt, standardOutput, standardError;
    std::optional<int> exitCode;
    bool executionMayHaveStarted{false}, terminationConfirmed{false}, outputTruncated{false};
};
struct ScriptPublicationSummary
{
    QString executionId, targetPath, status;
    bool published{false}, publicationMayHaveOccurred{false}, invocationStarted{false};
};
struct CommandResult
{
    std::optional<Error> error;
    std::optional<CommandExecutionSummary> execution;
    std::optional<ScriptPublicationSummary> publication;
};
struct CommandRecord
{
    QString executionId, commandId;
    SessionReference session;
    TaskPhase phase{TaskPhase::Preparing};
    bool complete{false};
    CommandResult result;
};
struct CommandPrompt
{
    ConfirmationTransaction transaction;
    ToolRequest request;
    QString resolvedTarget;
    QStringList reasons;
    bool targetExistsLocally{false}, detached{false};
};
class CommandRuntime
{
public:
    struct Delivery
    {
        // nullopt ends this MRTR round while its confirmation remains independently alive.
        std::function<void(const TaskId&, std::optional<CommandResult>)> complete;
        std::function<void(const TaskId&, std::chrono::milliseconds)> deadline;
        std::function<bool(const TaskId&, const CommandPrompt&)> confirm;
        std::function<void()> changed;
    };
    CommandRuntime(CommandSessionPort&, CommandTargetStore&, QString instance,
        Delivery, std::function<qint64()> now);
    ~CommandRuntime();
    CommandRuntime(const CommandRuntime&) = delete;
    CommandRuntime& operator=(const CommandRuntime&) = delete;
    void start(CommandInvocation);
    void confirm(const TaskId&, ConfirmationAnswer);
    void executionFinished(const CommandExecutionResult&);
    void publicationFinished(const ScriptPublicationResult&);
    void providerUnavailable(quint64 provider);
    void userInput(const SessionId&, quint64 generation);
    void cancel(const TaskId&);
    void expire(const TaskId&);
    void disconnect(const QString& connection);
    void revoke(const QString& client, const QString& session);
    void stop();
    void sweep();
    [[nodiscard]] bool idle() const;
    [[nodiscard]] bool busy(const SessionId&) const;
    [[nodiscard]] bool quarantined(const QString& target) const;
    [[nodiscard]] QList<CommandRecord> executionRecords() const;
    [[nodiscard]] QList<TargetProtection> protectedTargets() const;
    bool acknowledgeTarget(const QString& target, const QString& expectedExecution = {});
private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
} // namespace NovaTerm::Mcp
