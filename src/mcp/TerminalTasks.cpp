/** @file TerminalTasks.cpp @brief 复用会话执行事实的有界长任务关联与交付。 */
#include "TerminalTasks.h"
#include "TerminalInteraction.h"
#include "LogReadAdapter.h"
#include "McpAccess.h"
#include "McpProtocol.h"
#include "contracts/TerminalInputValidation.h"
#include "protocol/ToolCodec.h"
#include "session/SessionTerminalController.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QElapsedTimer>
#include <QIODevice>
#include <QPointer>
#include <QTimer>
#include <algorithm>

namespace NovaTerm::Mcp {
namespace {
QByteArray identity(const StartTerminalTaskRequest& request) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream << request.session.str() << request.epoch.str() << request.inputToken
        << qint64(request.timeout.count()) << quint32(request.actions.size());
    for (const auto& action : request.actions) {
        stream << quint8(action.index());
        std::visit([&stream](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, TextAction> || std::is_same_v<T, PasteAction>) stream << value.text;
            else if constexpr (std::is_same_v<T, CommandAction>) stream << value.command;
            else if constexpr (std::is_same_v<T, KeyAction>) {
                stream << value.key << quint32(value.modifiers.size());
                for (const auto modifier : value.modifiers) stream << quint8(modifier);
            }
        }, action);
    }
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
bool terminal(const QString& state) {
    return state == "completed" || state == "unknown" || state == "failed";
}
}

class TerminalTasks::Impl {
public:
    struct Record {
        QString id, client, session, epoch, operation, stopOperation, execution;
        QByteArray hash;
        quint64 grant{0}, user{0}, control{0};
        qint64 deadline{0};
        std::optional<qint64> ended;
        bool accepted{false}, revoked{false}, stopPreparing{false}, stopRequested{false};
        QString status{QStringLiteral("accepted")}, reason;
        QString submission{QStringLiteral("queued")};
        bool maySent{false};
        QJsonObject command, baseline, failure;
        QString startKey, stopKey;
    };
    using Row = std::shared_ptr<Record>;
    struct Waiter { Row row; Completion done; bool stopping{false}; };
    TerminalTasks* q;
    AccessStore& access;
    SessionDirectory& sessions;
    TerminalInteraction& interaction;
    QString instance;
    QByteArray logKey;
    QElapsedTimer clock;
    QTimer timer;
    QList<Row> records;
    QHash<QString, Waiter> waiters;
    bool stopped{false};

    Impl(TerminalTasks* owner, AccessStore& store, SessionDirectory& directory,
        TerminalInteraction& input, QString app, QByteArray key)
        : q(owner), access(store), sessions(directory), interaction(input),
          instance(std::move(app)), logKey(std::move(key)) {
        clock.start(); timer.setInterval(250);
        QObject::connect(&timer, &QTimer::timeout, q, [this] { sweep(true); });
        QObject::connect(&sessions, &SessionDirectory::invalidated, q,
            [this](const QString& session) { revoke({}, session); });
        QObject::connect(&access, &AccessStore::revoked, q,
            [this](const QString& client, const QString& session) { revoke(client, session); });
    }
    std::optional<SessionDirectory::Entry> entry(const Record& row) {
        const auto current = sessions.find(row.session);
        if (!current || !current->session || current->epoch != row.epoch) return {};
        return current;
    }
    bool permitted(const Record& row) {
        const auto current = entry(row);
        return !stopped && !row.revoked && current && access.canRead(row.client, *current)
            && access.version(row.client) == row.grant;
    }
    bool matches(const Record& row, SessionTerminalController* ctrl) const {
        const auto operation = ctrl->operation().value("id").toString();
        return row.accepted && ctrl->owner() == row.client
            && ctrl->userGeneration() == row.user && ctrl->controlGeneration() == row.control
            && (operation == row.operation || (!row.stopOperation.isEmpty() && operation == row.stopOperation))
            && (row.execution.isEmpty() || ctrl->executionId() == row.execution);
    }
    void end(const Row& row, const QString& state, const QString& reason = {}) {
        if (terminal(row->status)) return;
        row->status = state; row->reason = reason; row->ended = clock.elapsed();
    }
    void bind(const Row& row) {
        if (row->accepted || terminal(row->status)) return;
        const auto current = entry(*row);
        if (!current) return;
        auto* ctrl = current->session->terminalController();
        if (ctrl->operation().value("id").toString() != row->operation) return;
        row->accepted = true; row->control = ctrl->controlGeneration(); row->execution = ctrl->executionId();
    }
    void refresh(const Row& row, bool renew = false) {
        if (!row->accepted || terminal(row->status)) return;
        const auto current = entry(*row);
        if (!current || !permitted(*row)) { end(row, "unknown", "invalidated"); return; }
        auto* ctrl = current->session->terminalController();
        const auto observation = ctrl->observation(row->client);
        const auto operation = ctrl->operation();
        const bool same = matches(*row, ctrl);
        if (same) {
            row->submission = operation.value("status").toString();
            row->maySent |= operation.value("inputMayHaveBeenSent").toBool();
        }
        // 原始输入只能按现有 Controller 证据关联执行，不能猜测执行已开始。
        if (same && row->execution.isEmpty()) row->execution = ctrl->executionId();
        const auto command = observation.value("command").toObject();
        const auto recent = observation.value("lastCommand").toObject();
        if (!row->execution.isEmpty()) {
            if (command.value("executionId").toString() == row->execution) row->command = command;
            else if (recent.value("executionId").toString() == row->execution) row->command = recent;
            if (row->command.value("state") == "finished") { end(row, "completed"); return; }
        }
        if (!same) { end(row, "unknown", "control_changed"); return; }
        if (row->submission == "failed" || row->submission == "interrupted") {
            end(row, row->maySent ? "unknown" : "failed", "input_not_submitted"); return;
        }
        if (!row->execution.isEmpty() && row->command.value("state") == "unknown") {
            end(row, "unknown", "completion_unverified"); return;
        }
        if (clock.elapsed() >= row->deadline) {
            // 未知结果和释放控制仍由 Session 收口，不发送 Ctrl+C。
            end(row, "unknown", "deadline");
            ctrl->release(row->client, "expired"); return;
        }
        if (row->stopRequested) row->status = QStringLiteral("stop_requested");
        else row->status = row->submission == "queued" ? QStringLiteral("accepted") : QStringLiteral("running");
        if (renew) ctrl->touch(row->client);
    }
    QJsonObject data(const Row& row) const {
        const bool completed = row->status == "completed";
        return {{"taskId", row->id}, {"operationId", row->operation},
            {"sessionId", row->session}, {"epoch", row->epoch}, {"status", row->status},
            {"accepted", row->accepted}, {"completed", completed},
            {"executionId", row->execution.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(row->execution)},
            {"exitCode", completed ? row->command.value("exitCode") : QJsonValue(QJsonValue::Null)},
            {"terminationConfirmed", completed && row->command.value("terminationConfirmed").toBool()},
            {"evidenceKind", row->command.value("evidence").toString("none")},
            {"submissionStatus", row->submission}, {"inputMayHaveBeenSent", row->maySent},
            {"stopRequested", row->stopRequested}, {"reason", row->reason},
            {"baseline", row->baseline}, {"suggestedQueryMs", terminal(row->status) ? 0 : 250}};
    }
    QJsonObject result(const Row& row) {
        if (!permitted(*row)) return failure("SESSION_NOT_AVAILABLE", "Task authorization or connection changed.");
        const QPointer<TerminalTasks> guard(q);
        refresh(row);
        if (!guard) return failure("APP_UNAVAILABLE", "Task owner was destroyed; do not replay the task.");
        if (!row->accepted && !row->failure.isEmpty()) return row->failure;
        return success(data(row));
    }
    bool wait(const QString& key, const Row& row, Completion done, bool stopping = false) {
        const auto count = std::count_if(waiters.cbegin(), waiters.cend(),
            [&row](const Waiter& waiter) { return waiter.row == row; });
        if (waiters.contains(key) || count >= Budget::MaxWireCalls) {
            done(failure("BUSY", "Task delivery budget is full.")); return false;
        }
        waiters.insert(key, Waiter{row, std::move(done), stopping}); return true;
    }
    void deliver(const Row& row, bool stopping = false, const QJsonObject& error = {}) {
        QList<Completion> deliveries;
        for (auto it = waiters.begin(); it != waiters.end();) {
            if (it->row == row && it->stopping == stopping) {
                deliveries.append(std::move(it->done)); it = waiters.erase(it);
            } else ++it;
        }
        for (auto& done : deliveries) {
            const QPointer<TerminalTasks> guard(q);
            const auto response = !permitted(*row) ? failure("SESSION_NOT_AVAILABLE", "Task authorization changed.")
                : error.isEmpty() ? result(row) : error;
            if (!guard) return;
            done(response);
            if (!guard) return;
        }
    }
    Row byOperation(const QString& client, const QString& session, const QString& operation) const {
        for (const auto& row : records)
            if (row->client == client && row->session == session
                && (row->operation == operation || row->stopOperation == operation)) return row;
        return {};
    }
    void sweep(bool renew = false) {
        const QPointer<TerminalTasks> guard(q);
        const auto snapshot = records;
        for (const auto& row : snapshot) {
            refresh(row, renew);
            if (!guard) return;
        }
        const auto now = clock.elapsed();
        records.erase(std::remove_if(records.begin(), records.end(), [now](const Row& row) {
            return row->ended && now - *row->ended >= Budget::TerminalTaskRetention.count();
        }), records.end());
        if (records.isEmpty()) timer.stop();
    }
    void start(const QString& client, const QString& key, const StartTerminalTaskRequest& request,
        Completion done, const Capabilities::InvocationContext& context, const SessionDirectory::Entry& current) {
        const auto hash = identity(request);
        if (const auto existing = byOperation(client, current.id, request.operation.str())) {
            if (existing->hash != hash || existing->operation != request.operation.str() || existing->epoch != current.epoch) {
                done(failure("INPUT_SEQUENCE_CONFLICT", "operationId already identifies a different task.")); return;
            }
            if (existing->accepted || terminal(existing->status)) done(result(existing));
            else static_cast<void>(wait(key, existing, std::move(done)));
            return;
        }
        if (!access.canControlTerminal(client, current)) { done(failure("TERMINAL_PERMISSION_REQUIRED", "Terminal input permission is required.")); return; }
        const auto invalid = validateTerminalActions(request.actions);
        if (!invalid.isEmpty() || request.operation.isEmpty() || request.inputToken.isEmpty()
            || std::holds_alternative<ReleaseAction>(request.actions.front())
            || request.timeout.count() < 1 || request.timeout > Budget::TerminalTaskMaxTimeout) {
            done(failure("INVALID_ARGUMENT", invalid.isEmpty() ? "Invalid task input or timeout." : invalid)); return;
        }
        if (const auto error = interaction.validateNewInput(client,
            {request.session, request.epoch}, request.inputToken)) {
            done(ToolCodec::encodeFailure(*error)); return;
        }
        const auto count = std::count_if(records.cbegin(), records.cend(),
            [&client](const Row& row) { return row->client == client; });
        if (records.size() >= Budget::MaxTerminalTasks || count >= Budget::MaxTerminalTasksPerClient) {
            done(failure("BUSY", "Task index budget is full.")); return;
        }
        ReadContextRequest baseline; baseline.session = request.session; baseline.epoch = request.epoch;
        baseline.source = ContextSource::Log; baseline.start = LogInitialPosition::Tail;
        const auto page = readLogContext(access, sessions, instance, client, logKey, baseline);
        if (const auto* error = errorIfAny(page)) { done(ToolCodec::encodeFailure(*error)); return; }
        const auto row = std::make_shared<Record>();
        row->id = newId(); row->client = client; row->session = current.id; row->epoch = current.epoch;
        row->operation = request.operation.str(); row->hash = hash; row->grant = access.version(client);
        row->user = current.session->terminalController()->userGeneration();
        row->deadline = clock.elapsed() + request.timeout.count();
        row->baseline = std::get<QJsonObject>(page); row->startKey = "terminal-task/" + row->id + "/start";
        records.append(row); timer.start();
        if (!wait(key, row, std::move(done))) { records.removeOne(row); return; }
        InteractTerminalRequest input{request.session, request.epoch, request.inputToken,
            request.operation, request.actions, std::chrono::milliseconds(0), {}, ProjectionMode::Compact, {}, false};
        const QPointer<TerminalTasks> guard(q);
        interaction.call(client, row->startKey, ToolRequest{input},
            [this, guard, row](QJsonObject response) {
                if (!guard) return;
                if (!row->accepted) {
                    row->failure = response.value("ok").toBool()
                        ? failure("CONTROL_INVALID", "Input was not accepted for this task.") : response;
                    end(row, "failed", "input_not_accepted");
                }
                refresh(row);
                if (guard) deliver(row);
            }, {}, context, InputReplayPolicy::Reject, [this, guard, row] {
                if (guard) bind(row);
            });
        if (!guard) return;
        if (row->accepted) { refresh(row); if (guard) deliver(row); }
    }
    void query(const Row& row, const QueryTerminalTaskRequest& request, Completion done) {
        const QPointer<TerminalTasks> guard(q);
        auto response = result(row);
        if (!guard) return;
        if (response.value("ok").toBool() && request.sinceToken) {
            ReadContextRequest read; read.session = request.session; read.epoch = request.epoch;
            read.source = ContextSource::Log; read.sinceToken = request.sinceToken;
            auto value = response.value("data").toObject();
            const auto page = readLogContext(access, sessions, instance, row->client, logKey, read);
            if (const auto* error = errorIfAny(page)) value.insert("outputError", ToolCodec::encodeFailure(*error).value("error"));
            else value.insert("output", std::get<QJsonObject>(page));
            response = success(value);
        }
        done(std::move(response));
    }
    void stopTask(const QString& key, const Row& row, const StopTerminalTaskRequest& request,
        Completion done, const Capabilities::InvocationContext& context) {
        const QPointer<TerminalTasks> guard(q);
        refresh(row);
        if (!guard) return;
        if (request.operation.isEmpty()) { done(failure("INVALID_ARGUMENT", "operationId is required.")); return; }
        if (!row->stopOperation.isEmpty()) {
            if (row->stopOperation != request.operation.str()) {
                done(failure("INPUT_SEQUENCE_CONFLICT", "A stop operation already identifies this task.")); return;
            }
            if (row->stopPreparing) static_cast<void>(wait(key, row, std::move(done), true));
            else done(result(row));
            return;
        }
        const auto current = entry(*row);
        if (!current || !access.canControlTerminal(row->client, *current)) {
            done(failure("TERMINAL_PERMISSION_REQUIRED", "Terminal input permission is required.")); return;
        }
        if (terminal(row->status) || !matches(*row, current->session->terminalController())) {
            done(failure("CONTROL_INVALID", "The task no longer owns this terminal execution.")); return;
        }
        if (byOperation(row->client, row->session, request.operation.str())) {
            done(failure("INPUT_SEQUENCE_CONFLICT", "operationId is already used.")); return;
        }
        if (!wait(key, row, std::move(done), true)) return;
        row->stopOperation = request.operation.str(); row->stopPreparing = true;
        row->stopKey = "terminal-task/" + row->id + "/stop";
        ReadContextRequest read; read.session = SessionId{row->session}; read.epoch = Epoch{row->epoch};
        read.projection = ProjectionMode::Compact;
        interaction.call(row->client, row->stopKey + "/token", ToolRequest{read},
            [this, guard, row, context](QJsonObject observed) {
                if (!guard) return;
                refresh(row);
                if (!guard) return;
                const auto target = entry(*row);
                const auto token = observed.value("data").toObject().value("inputToken").toString();
                auto reject = [this, &row](QJsonObject error) {
                    row->stopPreparing = false; row->stopOperation.clear(); deliver(row, true, error);
                };
                if (!permitted(*row) || !target || !access.canControlTerminal(row->client, *target)
                    || terminal(row->status) || !matches(*row, target->session->terminalController())) {
                    reject(failure("CONTROL_INVALID", "The original task control changed.")); return;
                }
                auto* ctrl = target->session->terminalController();
                if (token.isEmpty()) { reject(observed.value("ok").toBool()
                    ? failure("CONTROL_INVALID", "No current input condition is available.") : observed); return; }
                if (!ctrl->guardNextInput(row->client, row->stopOperation, row->user, row->control, row->execution)) {
                    reject(failure("CONTROL_INVALID", "The original task execution changed.")); return;
                }
                InteractTerminalRequest input{SessionId{row->session}, Epoch{row->epoch}, token,
                    OperationId{row->stopOperation}, {KeyAction{"c", {KeyModifier::Ctrl}}},
                    std::chrono::milliseconds(0), {}, ProjectionMode::Compact, {}, false};
                if (!row->execution.isEmpty()) input.replyToExecution = ExecutionId{row->execution};
                interaction.call(row->client, row->stopKey, ToolRequest{input},
                    [this, guard, row](QJsonObject response) {
                        if (!guard) return;
                        row->stopPreparing = false;
                        const auto latest = entry(*row);
                        if (latest && latest->session->terminalController()->operation().value("id").toString() == row->stopOperation) {
                            row->stopRequested = true; refresh(row); if (guard) deliver(row, true);
                        } else {
                            row->stopOperation.clear();
                            deliver(row, true, response.value("ok").toBool()
                                ? failure("CONTROL_INVALID", "Stop input was not accepted.") : response);
                        }
                    }, {}, context);
            }, {}, context);
    }
    void call(const QString& client, const QString& key, const TerminalTaskRequest& request,
        Completion done, const Capabilities::InvocationContext& context) {
        const QPointer<TerminalTasks> guard(q);
        sweep();
        if (!guard) return;
        const auto reference = std::visit([](const auto& value) { return SessionReference{value.session, value.epoch}; }, request);
        const auto current = sessions.find(reference.session.str());
        if (stopped || !current || !current->session || !access.canRead(client, *current)) {
            done(failure("SESSION_NOT_AVAILABLE", "Session is not shared.")); return;
        }
        if (current->epoch != reference.epoch.str()) { done(failure("STALE_SESSION_EPOCH", "Refresh the session identity.")); return; }
        if (const auto* startRequest = std::get_if<StartTerminalTaskRequest>(&request)) {
            start(client, key, *startRequest, std::move(done), context, *current); return;
        }
        QString id, operation;
        if (const auto* queryRequest = std::get_if<QueryTerminalTaskRequest>(&request)) {
            if (queryRequest->task.has_value() == queryRequest->operation.has_value()) {
                done(failure("INVALID_ARGUMENT", "Use exactly one taskId or operationId.")); return;
            }
            id = queryRequest->task ? queryRequest->task->str() : QString{};
            operation = queryRequest->operation ? queryRequest->operation->str() : QString{};
        } else id = std::get<StopTerminalTaskRequest>(request).task.str();
        Row row;
        for (const auto& candidate : records)
            if (candidate->client == client && candidate->session == current->id && candidate->epoch == current->epoch
                && (operation.isEmpty() ? candidate->id == id : candidate->operation == operation)) { row = candidate; break; }
        if (!row || !permitted(*row)) { done(ToolCodec::encodeFailure(Error{ErrorCode::TaskNotAvailable,
            "Task is unavailable; do not automatically replay it."})); return; }
        if (const auto* queryRequest = std::get_if<QueryTerminalTaskRequest>(&request)) query(row, *queryRequest, std::move(done));
        else stopTask(key, row, std::get<StopTerminalTaskRequest>(request), std::move(done), context);
    }
    void revoke(const QString& client, const QString& session) {
        const QPointer<TerminalTasks> guard(q);
        const auto snapshot = records;
        for (const auto& row : snapshot) if ((client.isEmpty() || row->client == client)
            && (session.isEmpty() || row->session == session)) {
            const auto current = entry(*row); row->revoked = true;
            if (current && matches(*row, current->session->terminalController()))
                current->session->terminalController()->release(row->client, "revoked");
            if (!guard) return;
            end(row, "unknown", "revoked"); deliver(row);
            if (!guard) return;
            deliver(row, true);
            if (!guard) return;
        }
    }
    void stop() {
        if (stopped) return;
        const QPointer<TerminalTasks> guard(q);
        stopped = true; timer.stop(); revoke({}, {});
        if (!guard) return;
        for (const auto& row : records) {
            interaction.cancel(row->startKey); interaction.cancel(row->stopKey);
            interaction.cancel(row->stopKey + "/token");
        }
        waiters.clear();
    }
};

TerminalTasks::TerminalTasks(AccessStore& access, SessionDirectory& sessions, TerminalInteraction& interaction,
    QString instance, QByteArray key, QObject* parent) : QObject(parent),
    _impl(std::make_unique<Impl>(this, access, sessions, interaction, std::move(instance), std::move(key))) {}
TerminalTasks::~TerminalTasks() { _impl->stop(); }
void TerminalTasks::call(const QString& client, const QString& key, const TerminalTaskRequest& request,
    Completion done, const Capabilities::InvocationContext& context) {
    _impl->call(client, key, request, std::move(done), context);
}
void TerminalTasks::cancel(const QString& key) { _impl->waiters.remove(key); }
void TerminalTasks::revoke(const QString& client, const QString& session) { _impl->revoke(client, session); }
void TerminalTasks::stop() { _impl->stop(); }
QJsonObject TerminalTasks::capabilities(const QString& client, const QString& session) {
    const QPointer<TerminalTasks> guard(this);
    _impl->sweep();
    if (!guard) return {};
    const auto entry = _impl->sessions.find(session);
    if (!entry || !entry->session || !_impl->access.canRead(client, *entry)) return {};
    auto* ctrl = entry->session->terminalController();
    const auto input = _impl->interaction.capabilities(client, session).value("availability").toObject();
    const bool inputAvailable = !_impl->stopped && input.value("inputAvailable").toBool();
    const auto count = std::count_if(_impl->records.cbegin(), _impl->records.cend(),
        [&client](const Impl::Row& row) { return row->client == client; });
    const bool capacity = _impl->records.size() < Budget::MaxTerminalTasks && count < Budget::MaxTerminalTasksPerClient;
    const bool canStart = inputAvailable && capacity;
    return {{"canStart", canStart}, {"canObserve", true},
        {"completionEvidence", ctrl->integrated() ? "shell_integration" : "observed_prompt_or_unknown"},
        {"canVerifyExitCode", ctrl->integrated()}, {"silenceIsSuccess", false},
        {"transport", SessionDirectory::transportName(entry->kind)},
        {"stopAvailable", inputAvailable}, {"stopRequiresOriginalControl", true},
        {"disabledReason", capacity ? input.value("disabledReason") : QJsonValue("BUSY")},
        {"executionMode", "shared_terminal"}};
}
}
