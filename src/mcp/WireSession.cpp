/** @file WireSession.cpp @brief 与传输无关的 MCP 初始化、确认及请求关联。 */
#include "WireSession.h"
#include "ElicitationBroker.h"
#include "McpProtocol.h"
#include "protocol/ToolCodec.h"
#include <QElapsedTimer>
#include <QHash>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QTimer>
#include <cmath>
#include <utility>
#ifndef NOVATERM_VERSION
#define NOVATERM_VERSION "0.2.30"
#endif
namespace NovaTerm::Mcp
{
namespace
{
const QString MrtrProtocolVersion = QStringLiteral("2026-07-28");
QString key(const QJsonValue &id)
{
    return QString::fromUtf8(QJsonDocument(QJsonArray{id}).toJson(QJsonDocument::Compact));
}
bool validId(const QJsonValue &value)
{
    const auto number = value.toDouble();
    return value.isString() ||
           (value.isDouble() && std::isfinite(number) && std::floor(number) == number &&
            std::abs(number) <= 9007199254740991.0);
}
} // namespace
class WireSession::Impl
{
public:
    Impl(WireSession *owner, QString prefix, std::function<qint64()> monotonicClock)
        : q(owner), _prefix(std::move(prefix)), _monotonicClock(std::move(monotonicClock))
    {
        _clock.start();
        _timeouts.setInterval(std::chrono::milliseconds{100});
        QObject::connect(&_timeouts, &QTimer::timeout, q, [this] {
            const auto ids = _calls.keys();
            for (const auto &id : ids) {
                const auto call = _calls.value(id);
                if (call.deadline <= now()) {
                    if (call.sent)
                        emit q->backendEvent(BackendCancel{call.wireId});
                    finish(id,
                           failure(call.sent && (call.tool == "novaterm_execute_command" ||
                                                 call.tool == "novaterm_run_command" ||
                                                 call.tool == "novaterm_run_script" ||
                                                 call.tool == "novaterm_interact_terminal" ||
                                                 call.tool == "novaterm_request_elevation")
                                       ? "COMMAND_OUTCOME_UNKNOWN"
                                       : "DEADLINE_EXCEEDED",
                                   "Request deadline exceeded."));
                }
            }
            if (_calls.isEmpty())
                _timeouts.stop();
        });
    }
    qint64 now() const { return _monotonicClock ? _monotonicClock() : _clock.elapsed(); }
    struct Call {
        QString route;
        QJsonValue rpcId;
        QString wireId, tool;
        ToolCall typed;
        QByteArray confirmationDigest;
        qint64 deadline{0};
        QString requestState;
        QJsonObject inputResponses;
        bool mrtr{false}, formElicitation{false}, sent{false};
    };
    QJsonObject modernResult(QJsonObject result) const
    {
        if (!result.contains("resultType"))
            result.insert("resultType", "complete");
        result.insert(
            "_meta",
            QJsonObject{{"io.modelcontextprotocol/serverInfo",
                         QJsonObject{{"name", "novaterm"}, {"version", NOVATERM_VERSION}}}});
        return result;
    }
    void output(QJsonObject message,
                std::optional<bool> modern = std::nullopt,
                bool final = true,
                QString route = {})
    {
        if (modern.value_or(_requestMrtr) && message.value("result").isObject())
            message.insert("result", modernResult(message.value("result").toObject()));
        auto encoded = QJsonDocument(message).toJson(QJsonDocument::Compact);
        if (encoded.size() > MaxFrameBytes) {
            const auto id = message.value("id");
            if (message.value("result").toObject().contains("structuredContent")) {
                auto result =
                    toolResult(failure("RESPONSE_TOO_LARGE", "Response exceeds frame limit."));
                if (modern.value_or(_requestMrtr))
                    result = modernResult(std::move(result));
                message = rpcResult(id, result);
            } else {
                message = rpcError(id, -32603, "Response exceeds frame limit.");
            }
            encoded = QJsonDocument(message).toJson(QJsonDocument::Compact);
            if (encoded.size() > MaxFrameBytes) {
                // 极长请求 ID 也不能让错误替代包再次突破正文上限。
                message =
                    rpcError(QJsonValue(QJsonValue::Null), -32603, "Response exceeds frame limit.");
                encoded = QJsonDocument(message).toJson(QJsonDocument::Compact);
            }
        }
        const int errorCode = message.value("error").toObject().value("code").toInt();
        emit q->packet(route.isEmpty() ? _route : route, std::move(encoded), final, errorCode);
    }
    void finish(const QString &id, QJsonObject result)
    {
        if (!_calls.contains(id))
            return;
        const auto call = _calls.take(id);
        _elicitationBroker.forget(call.wireId);
        auto response = rpcResult(call.rpcId, toolResult(result));
        output(response, call.mrtr, true, call.route);
        if (_calls.isEmpty())
            _timeouts.stop();
    }
    void receive(const QJsonObject &message)
    {
        const auto method = message.value("method").toString();
        const auto id = message.value("id");
        const bool elicitationResponse =
            id.isString() && id.toString().startsWith("nt-elicit-") &&
            (message.value("result").isObject() || message.value("error").isObject());
        const auto validationError = rpcValidationError(message);
        if (!validationError.isEmpty()) {
            output(validationError);
            return;
        }
        if (elicitationResponse) {
            const auto resolution = _elicitationBroker.handleResponse(message);
            if (!resolution)
                return;
            emit q->backendEvent(BackendElicitationResult{resolution->wireId,
                ToolCodec::confirmationAnswer(resolution->action, resolution->confirmed)});
            return;
        }
        const auto params = message.value("params").toObject();
        const bool initializeRequest = method == "initialize";
        const auto requestMeta = params.value("_meta").toObject();
        const QString requestedVersion =
            initializeRequest
                ? params.value("protocolVersion").toString()
                : requestMeta.value("io.modelcontextprotocol/protocolVersion").toString();
        _requestMrtr = !initializeRequest && requestedVersion == MrtrProtocolVersion;
        _requestFormElicitation =
            _requestMrtr ? requestMeta.value("io.modelcontextprotocol/clientCapabilities")
                               .toObject()
                               .value("elicitation")
                               .toObject()
                               .value("form")
                               .isObject()
                         : _legacyFormElicitation;
        if (!initializeRequest && !requestedVersion.isEmpty() &&
            requestedVersion != MrtrProtocolVersion && requestedVersion != ProtocolVersion) {
            output(unsupportedProtocolError(id, requestedVersion));
            return;
        }
        if (!message.contains("id")) {
            if (method == "notifications/initialized" && _negotiated)
                _initialized = true;
            else if (method == "notifications/cancelled" && validId(params.value("requestId"))) {
                const auto identifier = key(params.value("requestId"));
                if (_calls.contains(identifier)) {
                    const auto call = _calls.take(identifier);
                    _elicitationBroker.forget(call.wireId);
                    if (call.sent)
                        emit q->backendEvent(BackendCancel{call.wireId});
                    emit q->completed(call.route);
                }
            }
            return;
        }
        if (method == "server/discover") {
            if (!_requestMrtr) {
                output(rpcError(id, -32601, "server/discover requires protocol 2026-07-28."));
                return;
            }
            output(
                rpcResult(
                    id,
                    {{"resultType", "complete"},
                     {"supportedVersions", QJsonArray{MrtrProtocolVersion, ProtocolVersion}},
                     {"capabilities", QJsonObject{{"tools", QJsonObject{{"listChanged", false}}}}},
                     {"_meta",
                      QJsonObject{
                          {"io.modelcontextprotocol/serverInfo",
                           QJsonObject{{"name", "novaterm"}, {"version", NOVATERM_VERSION}}}}},
                     {"instructions",
                      agentInstructions()},
                     {"ttlMs", 0},
                     {"cacheScope", "private"}}),
                true);
            return;
        }
        if (method == "initialize") {
            if (_negotiated || !params.value("protocolVersion").isString() ||
                !params.value("capabilities").isObject() ||
                !params.value("clientInfo").toObject().value("name").isString() ||
                !params.value("clientInfo").toObject().value("version").isString()) {
                output(rpcError(id, -32602, "Invalid initialize request."));
                return;
            }
            if (requestedVersion == MrtrProtocolVersion) {
                output(rpcError(id, -32601, "2026 clients must use server/discover."));
                return;
            }
            _negotiated = true;
            _legacyFormElicitation = params.value("capabilities")
                                         .toObject()
                                         .value("elicitation")
                                         .toObject()
                                         .value("form")
                                         .isObject();
            output(rpcResult(
                id,
                {{"protocolVersion", ProtocolVersion},
                 {"capabilities", QJsonObject{{"tools", QJsonObject{{"listChanged", false}}}}},
                 {"serverInfo", QJsonObject{{"name", "novaterm"}, {"version", NOVATERM_VERSION}}},
                 {"instructions",
                  agentInstructions()}}));
            return;
        }
        if (method == "ping") {
            output(rpcResult(id, {}));
            return;
        }
        if (!_requestMrtr && !_initialized) {
            output(rpcError(id, -32002, "MCP initialization has not completed."));
            return;
        }
        if (method == "tools/list") {
            output(rpcResult(id, {{"tools", ToolCodec::catalog()}}));
            return;
        }
        if (method != "tools/call") {
            output(rpcError(id, -32601, "Method not found."));
            return;
        }
        const auto name = params.value("name").toString();
        bool known = false;
        for (const auto &tool : ToolCodec::catalog())
            known |= tool.toObject().value("name") == name;
        if (!known) {
            output(rpcError(id, -32602, "Unknown tool."));
            return;
        }
        if (params.contains("arguments") && !params.value("arguments").isObject()) {
            output(
                rpcResult(id,
                          toolResult(failure("INVALID_ARGUMENT", "arguments must be an object."))));
            return;
        }
        const auto arguments = params.value("arguments").toObject();
        auto decoded = ToolCodec::decodeCall(name, arguments);
        if (const auto* error = errorIfAny(decoded)) {
            output(rpcResult(id, toolResult(ToolCodec::encodeFailure(*error))));
            return;
        }
        const auto identifier = key(id);
        if (_calls.contains(identifier)) {
            output(rpcError(id, -32600, "Duplicate active request id."));
            return;
        }
        if (_calls.size() >= Budget::MaxWireCalls) {
            output(rpcResult(id, toolResult(failure("BUSY", "Request queue is full.", true, 250))));
            return;
        }
        // 2025 在同一次请求内等待确认；执行及脚本写入的独立预算不可被等待消耗。
        // 2026 input_required 已结束本次请求，继续执行由新的 HTTP 请求承接。
        qint64 budget = name == "novaterm_execute_command" ? 7000 : 2000;
        if (name == "novaterm_run_command" || name == "novaterm_run_script") {
            budget = arguments.value("timeoutMs").toInt(5000) + WireDeadlineMargin.count();
            if (!_requestMrtr)
                budget += ConfirmationTimeout.count();
            if (name == "novaterm_run_script")
                budget += ScriptWriteTimeout.count();
        }
        if (name == "novaterm_read_context"
            && (arguments.value("responseMode") == "compact" || arguments.contains("waitMs")))
            budget = arguments.value("waitMs").toInt() + WireDeadlineMargin.count();
        if (name == "novaterm_interact_terminal") {
            const auto actions = arguments.value("actions").toArray();
            const auto type = actions.first().toObject().value("type").toString();
            budget = type == "release" ? 2000
                : arguments.value("waitMs").toInt(type == "command" ? MaxTerminalWaitMs : 1000)
                    + WireDeadlineMargin.count();
        }
        if (name == "novaterm_request_elevation")
            budget = arguments.value("waitMs").toInt(MaxTerminalWaitMs) + WireDeadlineMargin.count();
        if (name == "novaterm_clear_terminal") budget = 7000;
        if (name == "novaterm_wait_text") budget = arguments.value("waitMs").toInt() + WireDeadlineMargin.count();
        if (name == "novaterm_start_task" || name == "novaterm_query_task" || name == "novaterm_stop_task") budget = 7000;
        Call call;
        call.route = _route;
        call.rpcId = id;
        call.wireId = _prefix + QString::number(++_nextId);
        call.tool = name;
        call.typed = std::get<ToolCall>(std::move(decoded));
        const auto* terminalRequest = std::get_if<ToolRequest>(&call.typed);
        if (terminalRequest && std::holds_alternative<RunScriptRequest>(*terminalRequest)) {
            call.confirmationDigest = QCryptographicHash::hash(
                QJsonDocument(arguments).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
        }
        call.mrtr = _requestMrtr;
        call.formElicitation = _requestFormElicitation;
        call.deadline = now() + budget + (_available ? 0 : 3000);
        if (call.mrtr) {
            call.requestState = params.value("requestState").toString();
            call.inputResponses = params.value("inputResponses").toObject();
        }
        _calls.insert(identifier, std::move(call));
        _timeouts.start();
        if (_available)
            flush();
        else
            emit q->attachmentRequested();
    }
    void flush()
    {
        if (!_available)
            return;
        // 接线后端可同步返回；复制键与调用值，避免发射栈中移除 QHash 迭代器。
        const auto ids = _calls.keys();
        for (const auto &id : ids) {
            if (!_calls.contains(id) || _calls.value(id).sent)
                continue;
            const auto call = _calls.value(id);
            _calls[id].sent = true;
            if (const auto* target = std::get_if<TargetConnectionRequest>(&call.typed)) {
                emit q->backendEvent(BackendTargetConnectionCall{call.wireId, *target, !call.requestState.isEmpty()});
                continue;
            }
            if (const auto* transfer = std::get_if<FileTransferRequest>(&call.typed)) {
                emit q->backendEvent(BackendFileTransferCall{call.wireId, *transfer, !call.requestState.isEmpty()});
                continue;
            }
            if (const auto* files = std::get_if<ListFileResourcesRequest>(&call.typed)) {
                emit q->backendEvent(BackendFileResourcesCall{call.wireId, *files, !call.requestState.isEmpty()});
                continue;
            }
            if (const auto* serialTransfer = std::get_if<SerialFileTransferRequest>(&call.typed)) {
                emit q->backendEvent(BackendSerialFileTransferCall{call.wireId, *serialTransfer, !call.requestState.isEmpty()});
                continue;
            }
            if (const auto* targets = std::get_if<ListSerialFileTargetsRequest>(&call.typed)) {
                emit q->backendEvent(BackendSerialFileTargetsCall{call.wireId, *targets, !call.requestState.isEmpty()});
                continue;
            }
            if (const auto* targets = std::get_if<ListSavedTargetsRequest>(&call.typed)) {
                emit q->backendEvent(BackendSavedTargetsCall{call.wireId, *targets, !call.requestState.isEmpty()});
                continue;
            }
            if (const auto* wait = std::get_if<WaitTextRequest>(&call.typed)) {
                emit q->backendEvent(BackendWaitTextCall{call.wireId, *wait, !call.requestState.isEmpty()});
                continue;
            }
            if (const auto* task = std::get_if<TerminalTaskRequest>(&call.typed)) {
                emit q->backendEvent(BackendTaskCall{call.wireId, *task, !call.requestState.isEmpty()});
                continue;
            }
            if (const auto* clear = std::get_if<ClearRequest>(&call.typed)) {
                emit q->backendEvent(BackendClearCall{call.wireId, *clear, !call.requestState.isEmpty()});
                continue;
            }
            if (const auto* history = std::get_if<HistoryToolRequest>(&call.typed)) {
                emit q->backendEvent(BackendHistoryCall{call.wireId, *history,
                    !call.requestState.isEmpty()});
                continue;
            }
            emit q->backendEvent(BackendCall{call.wireId, std::get<ToolRequest>(call.typed), call.confirmationDigest,
                call.mrtr, call.formElicitation,
                call.mrtr && !call.requestState.isEmpty() ? call.requestState : QString(),
                call.mrtr && !call.requestState.isEmpty()
                    ? ToolCodec::confirmationAnswer(call.inputResponses) : std::nullopt});
        }
    }
    void backend(const QJsonObject &message)
    {
        const auto *packet = &message;
        if (_available && packet->value("op") == "input_required") {
            const auto wire = packet->value("id").toString();
            QString identifier;
            for (auto it = _calls.cbegin(); it != _calls.cend(); ++it)
                if (it->wireId == wire) {
                    identifier = it.key();
                    break;
                }
            if (identifier.isEmpty() || !_calls.value(identifier).mrtr)
                return;
            const QJsonObject schema{
                {"type", "object"},
                {"properties", QJsonObject{{"confirmed", QJsonObject{{"type", "boolean"}}}}},
                {"required", QJsonArray{"confirmed"}},
                {"additionalProperties", false}};
            const QJsonObject request{{"method", "elicitation/create"},
                                      {"params",
                                       QJsonObject{{"mode", "form"},
                                                   {"message", packet->value("message")},
                                                   {"requestedSchema", schema}}}};
            output(rpcResult(_calls.value(identifier).rpcId,
                             {{"resultType", "input_required"},
                              {"inputRequests", QJsonObject{{"confirm", request}}},
                              {"requestState", packet->value("requestState")}}),
                   true,
                   true,
                   _calls.value(identifier).route);
            _calls.remove(identifier);
            if (_calls.isEmpty())
                _timeouts.stop();
        } else if (_available && packet->value("op") == "result") {
            const auto wire = packet->value("id").toString();
            QString identifier;
            for (auto it = _calls.cbegin(); it != _calls.cend(); ++it)
                if (it->wireId == wire) {
                    identifier = it.key();
                    break;
                }
            if (!identifier.isEmpty())
                finish(identifier, packet->value("payload").toObject());
        } else if (_available && packet->value("op") == "elicitation") {
            const auto wire = packet->value("id").toString();
            QString identifier;
            for (auto it = _calls.cbegin(); it != _calls.cend(); ++it)
                if (it->wireId == wire) {
                    identifier = it.key();
                    break;
                }
            if (identifier.isEmpty())
                return;
            if (_calls.value(identifier).mrtr) {
                emit q->backendEvent(BackendCancel{wire});
                finish(identifier,
                       failure("CLIENT_CONFIRMATION_UNAVAILABLE",
                               "Client confirmation is unavailable."));
                return;
            }
            if (!_calls.value(identifier).formElicitation) {
                emit q->backendEvent(BackendCancel{wire});
                finish(identifier,
                       failure(QStringLiteral("CLIENT_CONFIRMATION_UNAVAILABLE"),
                               QStringLiteral("CLIENT_CONFIRMATION_UNAVAILABLE")));
                return;
            }
            const auto request =
                _elicitationBroker.requestConfirmation(wire, packet->value("message").toString());
            if (!request) {
                emit q->backendEvent(BackendCancel{wire});
                finish(identifier, failure("BUSY", "A confirmation is already pending."));
                return;
            }
            output(*request, false, false, _calls.value(identifier).route);
        }
    }
    WireSession *q;
    QString _prefix, _route;
    QHash<QString, Call> _calls;
    ElicitationBroker _elicitationBroker;
    QElapsedTimer _clock;
    std::function<qint64()> _monotonicClock;
    QTimer _timeouts;
    quint64 _nextId{0};
    bool _available{false}, _negotiated{false}, _initialized{false};
    bool _legacyFormElicitation{false}, _requestMrtr{false}, _requestFormElicitation{false};
};
WireSession::WireSession(QString prefix, QObject *parent, std::function<qint64()> monotonicClock)
    : QObject(parent), _impl(std::make_unique<Impl>(this, std::move(prefix), std::move(monotonicClock)))
{
}
WireSession::~WireSession() = default;
void WireSession::receiveBytes(QByteArray bytes, QString route)
{
    _impl->_route = std::move(route);
    if (bytes.size() > MaxRequestBytes) {
        _impl->output(rpcError(QJsonValue(QJsonValue::Null), -32600, "Request exceeds size limit."));
        return;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError) {
        _impl->output(rpcError(QJsonValue(QJsonValue::Null), -32700, "Invalid JSON."));
        return;
    }
    if (!document.isObject()) {
        _impl->output(
            rpcError(QJsonValue(QJsonValue::Null), -32600, "Batch requests are not supported."));
        return;
    }
    _impl->receive(document.object());
}
void WireSession::receiveMessage(QJsonObject message, QString route)
{
    _impl->_route = std::move(route);
    if (QJsonDocument(message).toJson(QJsonDocument::Compact).size() > MaxRequestBytes) {
        _impl->output(rpcError(QJsonValue(QJsonValue::Null), -32600, "Request exceeds size limit."));
        return;
    }
    _impl->receive(message);
}
void WireSession::receiveBackend(QJsonObject message) { _impl->backend(message); }
void WireSession::setBackendAvailable(bool available)
{
    _impl->_available = available;
    if (available)
        _impl->flush();
}
void WireSession::instanceUnavailable()
{
    _impl->_available = false;
    const auto ids = _impl->_calls.keys();
    for (const auto &id : ids) {
        const auto call = _impl->_calls.value(id);
        if (call.sent)
            emit backendEvent(BackendCancel{call.wireId});
        _impl->finish(id,
                      failure(call.sent && (call.tool == "novaterm_execute_command" ||
                                            call.tool == "novaterm_run_command" ||
                                            call.tool == "novaterm_run_script" ||
                                            call.tool == "novaterm_interact_terminal" ||
                                            call.tool == "novaterm_request_elevation")
                                  ? "COMMAND_OUTCOME_UNKNOWN"
                                  : "APP_UNAVAILABLE",
                              "Application disconnected; do not rerun automatically."));
    }
}
void WireSession::cancelAll()
{
    const auto calls = std::exchange(_impl->_calls, {});
    _impl->_timeouts.stop();
    for (const auto &call : calls) {
        _impl->_elicitationBroker.forget(call.wireId);
        if (call.sent)
            emit backendEvent(BackendCancel{call.wireId});
        emit completed(call.route);
    }
}
bool WireSession::initialized() const { return _impl->_initialized; }
void WireSession::failPending(QString code)
{
    const auto ids = _impl->_calls.keys();
    for (const auto &id : ids)
        _impl->finish(id, failure(code, code));
}
bool WireSession::hasPendingCalls() const { return !_impl->_calls.isEmpty(); }
} // namespace NovaTerm::Mcp
