#pragma once

#include "core/terminal/TerminalCore.h"
#include "SessionInputArbiter.h"
#include "SessionCommandCoordinator.h"
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <atomic>
#include <functional>

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
        if (_coordinator->isPromptReady()) {
            _prompt = _coordinator->promptGeneration(); _promptUser = userGeneration();
        }
        _idle.setSingleShot(true);
        connect(&_idle, &QTimer::timeout, this, [this] { release({}, QStringLiteral("expired")); });
        _promptTimer.setSingleShot(true);
        connect(&_promptTimer, &QTimer::timeout, this, [this] {
            if (_candidate && _candidateUser == _arbiter->userInputGeneration())
                ready(_candidate, std::nullopt, false);
            _candidate = 0;
        });
        connect(_arbiter, &SessionInputArbiter::userInputStarted, this, [this] {
            _prompt = 0; _candidate = 0; _promptTimer.stop();
            release({}, QStringLiteral("preempted"));
        });
        connect(_core, &TerminalCore::agentInputEncoded, this,
            [this](quint64 id, const QByteArray& bytes, bool valid) {
            if (!_pending || id != _encodingId) return;
            _pending = false;
            if (!valid || _owner.isEmpty() || !_arbiter->submitMcpInput(_lease, bytes)) {
                _operationStatus = QStringLiteral("failed");
                if (_commandActive) _commandState = QStringLiteral("unknown");
            } else {
                _operationStatus = QStringLiteral("submitted");
                _maySent = !bytes.isEmpty();
            }
            changed();
        });
    }
    ~SessionTerminalController() override { release({}); }

    std::function<void()> onChanged;
    const QString& owner() const { return _owner; }
    quint64 revision() const { return _revision; }
    quint64 controlGeneration() const { return _controlGeneration; }
    quint64 promptGeneration() const { return _prompt; }
    quint64 userGeneration() const { return _arbiter->userInputGeneration(); }
    bool pending() const { return _pending; }
    bool commandActive() const { return _commandActive; }
    QString executionId() const { return _execution; }
    QByteArray lineEnding() const { return _lineEnding; }
    void touch(const QString& client) { if (_owner == client && !client.isEmpty()) _idle.start(60000); }
    bool readyForCommand() const {
        return _prompt && !_commandActive && !_pending && !_core->isAlternateScreen()
            && _promptUser == _arbiter->userInputGeneration();
    }
    bool integrated() const { return _integrated; }
    bool acquire(const QString& client) {
        if (_owner == client) { touch(client); return true; }
        if (!_owner.isEmpty() || client.isEmpty()) return false;
        const auto lease = nextId();
        if (!_arbiter->acquireMcpLease(lease, _generation)) return false;
        _lease = lease; _owner = client; _controlState = QStringLiteral("controlled");
        ++_controlGeneration; touch(client); changed(); return true;
    }
    void release(const QString& client, const QString& reason = QStringLiteral("released")) {
        if (_owner.isEmpty() && reason == QStringLiteral("preempted")) {
            ++_controlGeneration; _controlState = reason; changed(); return;
        }
        if (!_owner.isEmpty() && (client.isEmpty() || client == _owner)) {
            _arbiter->releaseMcpLease(_lease);
            _owner.clear(); _lease = 0; _idle.stop(); _pending = false;
            _controlState = reason; ++_controlGeneration;
            if (_commandActive) {
                _commandActive = false; _commandState = QStringLiteral("unknown");
                if (_framer) { const auto remainder = _framer->endTransaction(); Q_UNUSED(remainder); }
            }
            if (_operationStatus == QStringLiteral("queued")) _operationStatus = QStringLiteral("interrupted");
            changed();
        }
    }
    bool submit(const QString& client, const QString& operation,
                std::vector<NovaTerm::TerminalInputAction> actions, bool command) {
        if (_pending || _owner != client || (command && !readyForCommand())) return false;
        _encodingId = nextId(); _operation = operation; _operationStatus = QStringLiteral("queued");
        _maySent = false; _pending = true;
        if (command) {
            _execution = QUuid::createUuid().toString(QUuid::WithoutBraces);
            _commandState = QStringLiteral("submitted"); _exit.reset(); _terminated = false;
            _commandActive = true;
            if (_framer) _framer->beginTransaction(_execution.toLatin1());
        }
        _prompt = 0; _candidate = 0; _promptTimer.stop();
        if (!_core->encodeAgentInput(_encodingId, std::move(actions))) {
            _pending = false; _operationStatus = QStringLiteral("failed");
            if (command) {
                _commandActive = false; _commandState = QStringLiteral("unknown");
                if (_framer) { const auto remainder = _framer->endTransaction(); Q_UNUSED(remainder); }
            }
            changed(); return false;
        }
        touch(client); changed(); return true;
    }
    void handleEvent(const InteractiveStreamEvent& event) {
        switch (event.kind) {
        case InteractiveStreamEventKind::PromptCandidate:
            _candidate = event.promptGeneration; _candidateUser = userGeneration();
            _promptTimer.start(150); break;
        case InteractiveStreamEventKind::PromptReady:
            ready(event.promptGeneration, std::nullopt, false); break;
        case InteractiveStreamEventKind::CommandFinishedAtPrompt:
            ready(event.promptGeneration, event.exitCode, true); break;
        case InteractiveStreamEventKind::CommandFinished:
            if (_commandActive) { finish(event.exitCode); changed(); } break;
        case InteractiveStreamEventKind::CommandStarted:
            if (_commandActive) { _commandState = QStringLiteral("running"); changed(); } break;
        case InteractiveStreamEventKind::ShellReset:
        case InteractiveStreamEventKind::FramingError:
            _prompt = 0; _candidate = 0; _promptTimer.stop();
            if (_commandActive) { _commandState = QStringLiteral("unknown"); _commandActive = false; }
            changed(); break;
        }
    }
    void bytes(const QByteArray&) {
        if (!_integrated) { _candidate = 0; _promptTimer.stop(); _prompt = 0; }
    }
    QJsonObject observation(const QString& client) const {
        const bool owned = _owner == client && !client.isEmpty();
        QJsonObject command{{"state", _commandState}, {"executionId", _execution},
            {"terminationConfirmed", _terminated}, {"exitCode", _exit ? QJsonValue(*_exit) : QJsonValue(QJsonValue::Null)},
            {"result", _exit ? (*_exit == 0 ? "success" : "failure") : "unknown"},
            {"evidence", _integrated ? "shell_integration" : (_prompt ? "device_prompt" : "none")},
            {"outputAttribution", "shared_terminal"}};
        QJsonObject value{{"command", command},
            {"control", QJsonObject{{"state", _owner.isEmpty() ? _controlState : QStringLiteral("controlled")},
                {"ownedByThisClient", owned}}},
            {"inputReadiness", QJsonObject{{"canSubmitCommand", readyForCommand()},
                {"canInteract", !_pending && (_owner.isEmpty() || owned) && !_arbiter->hasTransferLease()},
                {"confidence", _prompt ? (_integrated ? "verified" : "observed") : "unknown"},
                {"context", _core->isAlternateScreen() ? "alternate_screen" : (_prompt ? "shell" : "unknown")}}}};
        if (owned) value.insert("lastOperation", operation());
        return value;
    }
    QJsonObject operation() const {
        return {{"id", _operation}, {"status", _operationStatus}, {"inputMayHaveBeenSent", _maySent}};
    }
private:
    static quint64 nextId() { static std::atomic<quint64> id{quint64(1) << 63}; return ++id; }
    void changed() { ++_revision; if (onChanged) onChanged(); }
    void finish(std::optional<int> code) {
        _commandActive = false; _commandState = QStringLiteral("finished"); _terminated = true; _exit = code;
        if (_framer) { const auto remainder = _framer->endTransaction(); Q_UNUSED(remainder); }
    }
    void ready(quint64 generation, std::optional<int> code, bool completed) {
        if (_commandActive && !_pending && _operationStatus == QStringLiteral("submitted"))
            finish(completed ? code : std::nullopt);
        _prompt = generation; _promptUser = userGeneration(); changed();
    }
    QPointer<TerminalCore> _core;
    SessionInputArbiter* _arbiter;
    SessionCommandCoordinator* _coordinator;
    InteractiveStreamFramer* _framer;
    quint64 _generation, _lease{0}, _encodingId{0}, _revision{1}, _controlGeneration{1};
    quint64 _prompt{0}, _promptUser{0}, _candidate{0}, _candidateUser{0};
    bool _integrated, _pending{false}, _maySent{false}, _commandActive{false}, _terminated{false};
    QByteArray _lineEnding;
    QString _owner, _operation, _execution;
    QString _operationStatus{QStringLiteral("none")}, _commandState{QStringLiteral("none")};
    QString _controlState{QStringLiteral("idle")};
    std::optional<int> _exit;
    QTimer _idle, _promptTimer;
};
