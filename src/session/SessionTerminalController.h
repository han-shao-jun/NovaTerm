#pragma once

#include "core/terminal/TerminalCore.h"
#include "SessionInputArbiter.h"
#include "SessionCommandCoordinator.h"
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

/** Session-owned terminal input and command evidence, GUI thread only. */
class SessionTerminalController final : public QObject
{
public:
    SessionTerminalController(TerminalCore* core, SessionInputArbiter* arbiter,
        SessionCommandCoordinator* coordinator, InteractiveStreamFramer* framer,
        quint64 generation, bool integrated, QByteArray lineEnding, QObject* parent)
        : QObject(parent), _core(core), _arbiter(arbiter), _coordinator(coordinator),
          _framer(framer), _generation(generation), _integrated(integrated),
          _lineEnding(std::move(lineEnding))
    {
        _idle.setSingleShot(true);
        connect(&_idle, &QTimer::timeout, this, [this] { release({}, QStringLiteral("expired")); });
        _commandRetention.setParent(this);
        _commandRetention.setObjectName(QStringLiteral("commandRecordRetention"));
        _commandRetention.setSingleShot(true);
        connect(&_commandRetention, &QTimer::timeout, this, [this] {
            const auto expired = _recentCommand.value("executionId").toString();
            _recentCommand = {};
            if (!_commandActive && _execution == expired) clearCurrentRecord();
            changed();
        });
        connect(_coordinator, &SessionCommandCoordinator::promptReady, this,
            [this](quint64 generation) { ready(generation, std::nullopt, false); });
        connect(_arbiter, &SessionInputArbiter::userInputStarted, this, [this] {
            invalidatePromptEvidence();
            release({}, QStringLiteral("preempted"));
        });
        connect(_core, &TerminalCore::agentInputEncoded, this,
            [this](quint64 id, const QByteArray& bytes, bool valid) {
            if (!_pending || id != _encodingId) return;
            _pending = false;
            bool accepted = false;
            // Parser may have cleared the cache after encoding but before this
            // GUI callback. This successful check is the input admission point:
            // an invalidation already visible here rejects the queued input.
            // Once admitted, this GUI submission may overlap a later Parser
            // clear; its GUI completion notification runs after this callback.
            const bool taskCurrent = !_encodingTaskGuard || (matches(*_encodingTaskGuard)
                && _operation == _encodingTaskGuard->operation);
            _encodingTaskGuard.reset();
            const bool cacheCurrent = _encodingCacheGeneration == _core->cacheGeneration();
            if (!cacheCurrent) invalidatePromptEvidence();
            if (valid && cacheCurrent && taskCurrent && !_owner.isEmpty()) {
                if (!bytes.isEmpty()) {
                    const auto previousEvidence = _promptEvidenceRevision;
                    invalidatePromptEvidence();
                    // Allow rollback of our own invalidation, but never a newer
                    // incoming event, if the Arbiter rejects without writing.
                    if (_savedPromptEvidenceRevision == previousEvidence)
                        _savedPromptEvidenceRevision = _promptEvidenceRevision;
                    _coordinator->invalidatePromptReadiness();
                }
                _operationStatus = QStringLiteral("submitted");
                _maySent = !bytes.isEmpty();
                accepted = _arbiter->submitMcpInput(_lease, bytes);
            }
            if (!accepted) {
                clearCandidateCommand();
                _maySent = false;
                _operationStatus = QStringLiteral("failed");
                if (_pendingStartsCommand && _commandActive) {
                    _commandActive = false; _commandState = QStringLiteral("unknown");
                    if (_framer) { const auto remainder = _framer->endTransaction(); Q_UNUSED(remainder); }
                }
                restorePrompt();
            } else {
                if (bytes.isEmpty()) restorePrompt();
                else {
                    _savedPrompt = 0; _savedPromptUser = 0;
                    _savedPromptEvidenceRevision = 0;
                }
            }
            changed();
        });
    }
    ~SessionTerminalController() override { release({}); }

    std::function<void()> onChanged;
    const QString& owner() const { return _owner; }
    quint64 revision() const { return _revision; }
    quint64 controlGeneration() const { return _controlGeneration; }
    quint64 promptGeneration() const { return _coordinator->promptGeneration(); }
    quint64 userGeneration() const { return _arbiter->userInputGeneration(); }
    bool pending() const { return _pending; }
    bool commandActive() const { return _commandActive; }
    QString executionId() const { return _execution; }
    QByteArray lineEnding() const { return _lineEnding; }
    void touch(const QString& client) { if (_owner == client && !client.isEmpty()) _idle.start(60000); }
    bool readyForCommand() const {
        return _coordinator->isPromptReady() && !_commandActive && !_pending
            && !_core->isAlternateScreen()
            && !_typedSincePrompt && !_expectedPrompts && !_inputContextUncertain;
    }
    bool integrated() const { return _integrated; }
    /** @brief 将下一次输入及编码回调绑定到现有任务的控制条件。 */
    bool guardNextInput(const QString& client, const QString& operation,
        quint64 user, quint64 control, const QString& execution) {
        TaskInputGuard guard{client, operation, execution, user, control};
        if (!matches(guard)) return false;
        _nextTaskGuard = std::move(guard);
        return true;
    }
    bool acquire(const QString& client) {
        if (_owner == client) { touch(client); return true; }
        if (!_owner.isEmpty() || client.isEmpty()) return false;
        const auto lease = nextId();
        if (!_arbiter->acquireMcpLease(lease, _generation)) return false;
        _lease = lease; _owner = client; _controlState = QStringLiteral("controlled");
        ++_controlGeneration; touch(client); changed(); return true;
    }
    void release(const QString& client, const QString& reason = QStringLiteral("released"),
                 const QString& operation = {}) {
        if (_owner.isEmpty() && reason == QStringLiteral("preempted")) {
            ++_controlGeneration; _controlState = reason; changed(); return;
        }
        if (!_owner.isEmpty() && (client.isEmpty() || client == _owner)) {
            _arbiter->releaseMcpLease(_lease);
            _owner.clear(); _lease = 0; _idle.stop(); _pending = false;
            _nextTaskGuard.reset(); _encodingTaskGuard.reset();
            _savedPrompt = 0; _savedPromptUser = 0;
            _savedPromptEvidenceRevision = 0;
            clearCandidateCommand();
            _controlState = reason; ++_controlGeneration;
            if (_commandActive) {
                _commandActive = false; _commandState = QStringLiteral("unknown");
                if (_framer) { const auto remainder = _framer->endTransaction(); Q_UNUSED(remainder); }
            }
            if (_operationStatus == QStringLiteral("queued")) _operationStatus = QStringLiteral("interrupted");
            changed();
        }
        if (!operation.isEmpty()) {
            _operation = operation; _operationStatus = QStringLiteral("submitted");
            _maySent = false; _acceptedActionCount = 1; changed();
        }
    }
    bool submit(const QString& client, const QString& operation,
                std::vector<NovaTerm::TerminalInputAction> actions, bool command,
                int acceptedActionCount = -1,
                std::optional<quint64> expectedCacheGeneration = std::nullopt) {
        auto guard = std::exchange(_nextTaskGuard, std::nullopt);
        if (guard && guard->operation != operation) guard.reset();
        if (guard && !matches(*guard)) return false;
        const auto cacheGeneration = expectedCacheGeneration.value_or(_core->cacheGeneration());
        if (cacheGeneration != _core->cacheGeneration()
            || _pending || _owner != client || (command && !readyForCommand())) return false;
        _encodingId = nextId(); _operation = operation; _operationStatus = QStringLiteral("queued");
        _encodingCacheGeneration = cacheGeneration;
        _encodingTaskGuard = std::move(guard);
        _pendingStartsCommand = command;
        _maySent = false; _pending = true;
        _acceptedActionCount = acceptedActionCount >= 0 ? acceptedActionCount : int(actions.size());
        _savedPrompt = _coordinator->promptGeneration();
        _savedPromptUser = _coordinator->promptUserGeneration();
        _savedPromptEvidenceRevision = _promptEvidenceRevision;
        _savedTypedSincePrompt = _typedSincePrompt;
        _savedExpectedPrompts = _expectedPrompts;
        _savedInputContextUncertain = _inputContextUncertain;
        _savedInputWasCr = _inputWasCr;
        if (!command && !_commandActive && std::any_of(actions.begin(), actions.end(),
            [](const auto& action) {
                return !action.text.empty() || (action.kind == NovaTerm::TerminalInputAction::Kind::Key
                    && action.key != NovaTerm::InputKey::None);
            })) clearCurrentRecord();
        if (command) {
            clearCandidateCommand();
            _execution = QUuid::createUuid().toString(QUuid::WithoutBraces);
            _commandState = QStringLiteral("submitted"); _exit.reset(); _terminated = false;
            _commandEvidence = _integrated ? QStringLiteral("shell_integration") : QStringLiteral("device_prompt");
            _commandActive = true;
            if (_framer) _framer->beginTransaction(_execution.toLatin1());
        } else if (_integrated && !_commandActive) {
            const auto previousPending = _expectedPrompts;
            int submittedLines = 0;
            bool submittedText = false;
            const auto submitLine = [&] {
                submittedText |= _typedSincePrompt;
                _typedSincePrompt = false;
                if (_expectedPrompts < 64) ++_expectedPrompts;
                else _inputContextUncertain = true;
                ++submittedLines;
            };
            for (const auto& action : actions) {
                if (action.kind == NovaTerm::TerminalInputAction::Kind::Key) {
                    _inputWasCr = false;
                    if (action.key == NovaTerm::InputKey::Enter) submitLine();
                    else if (action.modifiers == 4U && action.key == NovaTerm::InputKey::None
                        && (action.text == "c" || action.text == "C")) {
                        // Ctrl+C 放弃此前未提交输入。这里只丢弃本地输入跟踪，
                        // 仍须收到新的可信提示符才能恢复 command 就绪。
                        _typedSincePrompt = false; _expectedPrompts = 0;
                        _inputContextUncertain = false; clearCandidateCommand();
                    }
                    else if (action.modifiers & (4U | 2U)) _inputContextUncertain = true;
                    else if (action.key != NovaTerm::InputKey::None || !action.text.empty())
                        _typedSincePrompt = true;
                } else {
                    for (const auto byte : action.text) {
                        if (byte == '\r') { submitLine(); _inputWasCr = true; }
                        else if (byte == '\n') {
                            if (!_inputWasCr) submitLine();
                            _inputWasCr = false;
                        } else { _typedSincePrompt = true; _inputWasCr = false; }
                    }
                }
            }
            if (submittedLines || _typedSincePrompt) clearCandidateCommand();
            if (!previousPending && submittedLines == 1 && submittedText
                && !_typedSincePrompt && !_inputContextUncertain && _framer) {
                // Merely arm framing. A record is created only when integration
                // produces a correlated start/completion, not from the newline.
                _candidateExecution = QUuid::createUuid().toString(QUuid::WithoutBraces);
                _framer->beginTransaction(_candidateExecution.toLatin1());
            }
        }
        _coordinator->invalidatePromptReadiness();
        if (!_core->encodeAgentInput(_encodingId, std::move(actions))) {
            clearCandidateCommand();
            _pending = false; _operationStatus = QStringLiteral("failed");
            if (command) {
                _commandActive = false; _commandState = QStringLiteral("unknown");
                if (_framer) { const auto remainder = _framer->endTransaction(); Q_UNUSED(remainder); }
            }
            restorePrompt();
            changed(); return false;
        }
        touch(client); changed(); return true;
    }
    void handleEvent(const InteractiveStreamEvent& event) {
        switch (event.kind) {
        case InteractiveStreamEventKind::PromptCandidate:
            ++_promptEvidenceRevision; break;
        case InteractiveStreamEventKind::PromptReady:
            ready(event.promptGeneration, std::nullopt, false); break;
        case InteractiveStreamEventKind::CommandFinishedAtPrompt:
            ready(event.promptGeneration, event.exitCode, true); break;
        case InteractiveStreamEventKind::CommandFinished:
            adoptCandidateCommand();
            if (_commandActive) { finish(event.exitCode); changed(); } break;
        case InteractiveStreamEventKind::CommandStarted:
            adoptCandidateCommand();
            if (_commandActive) { _commandState = QStringLiteral("running"); changed(); } break;
        case InteractiveStreamEventKind::ShellReset:
        case InteractiveStreamEventKind::FramingError:
            invalidatePromptEvidence();
            clearCandidateCommand();
            _typedSincePrompt = false;
            _expectedPrompts = 0; _inputContextUncertain = false; _inputWasCr = false;
            // 可信帧重置同时使原始输入任务与命令失效，
            // 排队中的旧任务中断不得落入新的 Shell。
            if (_owner.isEmpty()) { ++_controlGeneration; _controlState = QStringLiteral("shell_reset"); }
            else release({}, QStringLiteral("shell_reset"));
            if (_commandActive) { _commandState = QStringLiteral("unknown"); _commandActive = false; }
            changed(); break;
        }
    }
    void bytes(const QByteArray& data) {
        if (!_integrated && !data.isEmpty()) invalidatePromptEvidence();
    }
    QJsonObject observation(const QString& client) const {
        const bool owned = _owner == client && !client.isEmpty();
        QJsonObject value{{"command", commandRecord()},
            {"control", QJsonObject{{"state", _owner.isEmpty() ? _controlState : QStringLiteral("controlled")},
                {"ownedByThisClient", owned}}},
            {"inputReadiness", QJsonObject{{"canSubmitCommand", readyForCommand()},
                {"canInteract", !_pending && (_owner.isEmpty() || owned) && !_arbiter->hasTransferLease()},
                {"confidence", promptGeneration() ? (_integrated ? "verified" : "observed") : "unknown"},
                {"context", _core->isAlternateScreen() ? "alternate_screen" : (promptGeneration() ? "shell" : "unknown")}}}};
        if (owned) value.insert("lastOperation", operation());
        if (!_recentCommand.isEmpty()) value.insert("lastCommand", _recentCommand);
        return value;
    }
    QJsonObject operation() const {
        return {{"id", _operation}, {"status", _operationStatus}, {"inputMayHaveBeenSent", _maySent},
            {"acceptedActionCount", _acceptedActionCount}};
    }
private:
    struct TaskInputGuard {
        QString client, operation, execution;
        quint64 user{0}, control{0};
    };
    bool matches(const TaskInputGuard& guard) const {
        return _owner == guard.client && userGeneration() == guard.user
            && _controlGeneration == guard.control && _execution == guard.execution
            && (guard.execution.isEmpty() || _commandActive);
    }
    static quint64 nextId() { static std::atomic<quint64> id{quint64(1) << 63}; return ++id; }
    void changed() { ++_revision; if (onChanged) onChanged(); }
    void invalidatePromptEvidence() {
        ++_promptEvidenceRevision;
        _coordinator->invalidatePromptReadiness();
    }
    void restorePrompt() {
        // 编码等待期间的新输出/reset/提示符优先于此前保存的证据。
        if (!_commandActive && _savedPromptEvidenceRevision == _promptEvidenceRevision
            && _savedPromptUser == userGeneration()) {
            _coordinator->restorePromptEvidence(_savedPrompt, _savedPromptUser);
            _typedSincePrompt = _savedTypedSincePrompt;
            _expectedPrompts = _savedExpectedPrompts;
            _inputContextUncertain = _savedInputContextUncertain;
            _inputWasCr = _savedInputWasCr;
        }
        _savedPrompt = 0;
        _savedPromptUser = 0;
        _savedPromptEvidenceRevision = 0;
    }
    void finish(std::optional<int> code) {
        _commandActive = false; _commandState = QStringLiteral("finished"); _terminated = true; _exit = code;
        _recentCommand = commandRecord();
        _commandRetention.start(std::chrono::milliseconds(std::chrono::minutes(10)));
        if (_framer) { const auto remainder = _framer->endTransaction(); Q_UNUSED(remainder); }
    }
    QJsonObject commandRecord() const {
        return {{"state", _commandState}, {"executionId", _execution},
            {"terminationConfirmed", _terminated}, {"exitCode", _exit ? QJsonValue(*_exit) : QJsonValue(QJsonValue::Null)},
            {"result", _exit ? (*_exit == 0 ? "success" : "failure") : "unknown"},
            {"evidence", _commandEvidence}, {"outputAttribution", "shared_terminal"}};
    }
    void clearCurrentRecord() {
        _execution.clear(); _commandState = QStringLiteral("none");
        _exit.reset(); _terminated = false; _commandEvidence = QStringLiteral("none");
    }
    void clearCandidateCommand() {
        if (!_candidateExecution.isEmpty() && _framer) {
            const auto remainder = _framer->endTransaction(); Q_UNUSED(remainder);
        }
        _candidateExecution.clear();
    }
    void adoptCandidateCommand() {
        if (_candidateExecution.isEmpty() || _pending || _operationStatus != "submitted") return;
        _execution = std::exchange(_candidateExecution, QString{});
        _commandActive = true; _commandState = QStringLiteral("running");
        _exit.reset(); _terminated = false; _commandEvidence = QStringLiteral("shell_integration");
    }
    void ready(quint64 generation, std::optional<int> code, bool completed) {
        ++_promptEvidenceRevision;
        if (_integrated && !_pending) {
            if (_expectedPrompts) --_expectedPrompts;
            if (_typedSincePrompt || _expectedPrompts || _inputContextUncertain) {
                clearCandidateCommand(); invalidatePromptEvidence(); changed(); return;
            }
        }
        if (completed) adoptCandidateCommand();
        if (_commandActive && !_pending && _operationStatus == QStringLiteral("submitted"))
            finish(completed ? code : std::nullopt);
        clearCandidateCommand();
        if (!_pending) _typedSincePrompt = false;
        _coordinator->notePromptReady(generation, userGeneration());
        changed();
    }
    QPointer<TerminalCore> _core;
    SessionInputArbiter* _arbiter;
    SessionCommandCoordinator* _coordinator;
    InteractiveStreamFramer* _framer;
    quint64 _generation, _lease{0}, _encodingId{0}, _revision{1}, _controlGeneration{1};
    quint64 _encodingCacheGeneration{0};
    quint64 _savedPromptUser{0};
    quint64 _savedPrompt{0};
    quint64 _promptEvidenceRevision{1}, _savedPromptEvidenceRevision{0};
    bool _integrated, _pending{false}, _maySent{false}, _commandActive{false}, _terminated{false};
    bool _pendingStartsCommand{false};
    int _acceptedActionCount{0};
    QByteArray _lineEnding;
    QString _owner, _operation, _execution;
    QString _candidateExecution;
    bool _typedSincePrompt{false};
    int _expectedPrompts{0};
    bool _inputContextUncertain{false}, _inputWasCr{false};
    bool _savedTypedSincePrompt{false}, _savedInputContextUncertain{false}, _savedInputWasCr{false};
    int _savedExpectedPrompts{0};
    QString _commandEvidence{QStringLiteral("none")};
    QJsonObject _recentCommand;
    QString _operationStatus{QStringLiteral("none")}, _commandState{QStringLiteral("none")};
    QString _controlState{QStringLiteral("idle")};
    std::optional<int> _exit;
    QTimer _idle, _commandRetention;
    std::optional<TaskInputGuard> _nextTaskGuard, _encodingTaskGuard;
};
