/** @file HttpMcpEndpoint.cpp
 *  @brief HTTP 生命周期、会话隔离与 MCP wire 路由；不操作网络 socket。
 */
#include "HttpMcpEndpoint.h"
#include "McpProtocol.h"
#include "WireSession.h"
#include <QDateTime>
#include <QHash>
#include <QJsonDocument>
#include <QPointer>
#include <QSet>
#include <QTimer>

namespace NovaTerm::Mcp
{
namespace
{
constexpr auto ModernVersion = "2026-07-28";
constexpr qint64 ContextLifetimeMs = 10 * 60 * 1000;
constexpr int MaxExchanges = 32;
qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }
} // namespace

class HttpMcpEndpoint::Impl
{
public:
    struct Context {
        QString client;
        QPointer<WireSession> wire;
        bool modern{false};
        qint64 touched{0};
    };
    struct Exchange {
        QString peer;
        QPointer<WireSession> wire;
        QJsonObject headers;
        bool modern{false};
        bool sse{false};
        bool initializing{false};
    };
    Impl(HttpMcpEndpoint *owner, Backend callbacks)
        : q(owner), backend(std::move(callbacks)), io(owner)
    {
        QObject::connect(&io, &HttpMcpServer::listening, q, &HttpMcpEndpoint::listening);
        QObject::connect(
            &io,
            &HttpMcpServer::request,
            q,
            [this](const QString &id, const QJsonObject &envelope) { request(id, envelope); });
        QObject::connect(&io, &HttpMcpServer::disconnected, q, [this](const QString &id) {
            dropped(id);
        });
        timer.setInterval(30000);
        QObject::connect(&timer, &QTimer::timeout, q, [this] {
            const auto peers = contexts.keys();
            for (const auto &peer : peers) {
                const auto context = contexts.value(peer);
                bool active = context.wire && context.wire->hasPendingCalls();
                for (const auto &exchange : exchanges)
                    active = active || exchange.peer == peer;
                if (!active && now() - context.touched > ContextLifetimeMs)
                    close(peer);
            }
        });
    }
    void reply(const QString &id,
               int status,
               const QJsonObject &message = {},
               const QJsonObject &headers = {})
    {
        const auto bytes = message.isEmpty()
                               ? QByteArray{}
                               : QJsonDocument(message).toJson(QJsonDocument::Compact);
        static_cast<void>(io.respond(id, status, bytes, headers));
    }
    WireSession *createWire(const QString &peer)
    {
        auto *wire = new WireSession(newId() + '-', q);
        wires.insert(wire);
        QObject::connect(wire, &QObject::destroyed, q, [this, wire] { wires.remove(wire); });
        QObject::connect(wire,
                         &WireSession::backendEvent,
                         q,
                         [this, wire, peer](const BackendEvent &event) {
            if (const auto *call = std::get_if<BackendCall>(&event))
                jobs.insert(peer + '/' + call->id, wire);
            else if (const auto *history = std::get_if<BackendHistoryCall>(&event))
                jobs.insert(peer + '/' + history->id, wire);
            else if (const auto *clear = std::get_if<BackendClearCall>(&event))
                jobs.insert(peer + '/' + clear->id, wire);
            else if (const auto *wait = std::get_if<BackendWaitTextCall>(&event))
                jobs.insert(peer + '/' + wait->id, wire);
            else if (const auto *task = std::get_if<BackendTaskCall>(&event))
                jobs.insert(peer + '/' + task->id, wire);
            backend.receive(peer, event);
            if (const auto *cancel = std::get_if<BackendCancel>(&event))
                jobs.remove(peer + '/' + cancel->id);
        });
        QObject::connect(
            wire,
            &WireSession::packet,
            q,
            [this](const QString &route, const QByteArray &packet, bool final, int errorCode) {
            const auto found = exchanges.constFind(route);
            if (found == exchanges.cend())
                return;
            const auto exchange = found.value();
            auto headers = exchange.headers;
            if (exchange.initializing && errorCode != 0)
                headers.remove("Mcp-Session-Id");
            const int status = exchange.modern && errorCode == -32601 ? 404 : 200;
            const bool queued = io.respond(route, status, packet, headers, exchange.sse, final);
            if (final) {
                finish(route);
                if (exchange.initializing && (errorCode != 0 || !queued))
                    close(exchange.peer);
            } else if (!queued)
                dropped(route);
        });
        QObject::connect(wire, &WireSession::completed, q, [this](const QString &route) {
            const auto exchange = exchanges.value(route);
            if (exchanges.contains(route)) {
                static_cast<void>(io.respond(route,
                                             exchange.sse ? 200 : 202,
                                             {},
                                             exchange.headers,
                                             exchange.sse,
                                             true));
                finish(route);
            }
        });
        wire->setBackendAvailable(true);
        return wire;
    }
    void finish(const QString &route)
    {
        const auto exchange = exchanges.take(route);
        if (exchange.modern && exchange.wire) {
            removeJobs(exchange.wire);
            exchange.wire->deleteLater();
        }
    }
    void removeJobs(WireSession *wire)
    {
        for (auto it = jobs.begin(); it != jobs.end();) {
            if (it.value() == wire)
                it = jobs.erase(it);
            else
                ++it;
        }
    }
    void dropped(const QString &route)
    {
        const auto exchange = exchanges.take(route);
        if (exchange.modern && exchange.wire) {
            // 2026 每请求断流即取消；2025 的 HTTP 断开保留逻辑会话。
            exchange.wire->cancelAll();
            removeJobs(exchange.wire);
            exchange.wire->deleteLater();
        }
    }
    bool attach(const QString &peer, const QString &client, bool modern)
    {
        if (contexts.contains(peer))
            return contexts.value(peer).client == client;
        if (contexts.size() >= MaxClients || !backend.attach(peer, client))
            return false;
        Context context{client, {}, modern, now()};
        contexts.insert(peer, context);
        return true;
    }
    void close(const QString &peer)
    {
        if (!contexts.contains(peer))
            return;
        const auto context = contexts.take(peer);
        QSet<WireSession *> selected;
        if (context.wire)
            selected.insert(context.wire);
        for (const auto &exchange : exchanges)
            if (exchange.peer == peer && exchange.wire)
                selected.insert(exchange.wire);
        for (auto *wire : selected) {
            wire->instanceUnavailable();
            wire->cancelAll();
            removeJobs(wire);
            wire->deleteLater();
        }
        const auto routes = exchanges.keys();
        for (const auto &route : routes)
            if (exchanges.value(route).peer == peer) {
                const auto exchange = exchanges.take(route);
                static_cast<void>(
                    io.respond(route, exchange.sse ? 200 : 202, {}, {}, exchange.sse, true));
            }
        backend.detach(peer);
    }
    void request(const QString &route, const QJsonObject &envelope)
    {
        if (stopping) {
            reply(route, 503);
            return;
        }
        const auto headers = envelope.value("headers").toObject();
        const auto authorization = headers.value("authorization").toString().toLatin1();
        const auto client = authorization.startsWith("Bearer ")
                                ? backend.authenticate(authorization.mid(7))
                                : QString{};
        if (client.isEmpty()) {
            reply(route, 401, {}, {{"WWW-Authenticate", "Bearer"}});
            return;
        }
        const QString method = envelope.value("method").toString();
        const auto message = envelope.value("message").toObject();
        const auto params = message.value("params").toObject();
        const auto protocol = headers.value("mcp-protocol-version").toString();
        const auto rpcMethod = message.value("method").toString();
        const bool modern = protocol == QLatin1String(ModernVersion);
        if (!protocol.isEmpty() && protocol != QLatin1String(ProtocolVersion) && !modern) {
            reply(route, 400, unsupportedProtocolError(message.value("id"), protocol));
            return;
        }
        const QString session = headers.value("mcp-session-id").toString();
        if (method == "GET") {
            reply(route, 405, {}, {{"Allow", "POST, DELETE"}});
            return;
        }
        if (method == "DELETE") {
            if (modern) {
                reply(route, 405, {}, {{"Allow", "POST"}});
                return;
            }
            if (!contexts.contains(session) || contexts.value(session).modern ||
                contexts.value(session).client != client) {
                reply(route, 404);
                return;
            }
            close(session);
            reply(route, 200);
            return;
        }
        if (method != "POST") {
            reply(route, 405, {}, {{"Allow", "POST"}});
            return;
        }
        if (headers.value("content-type")
                .toString()
                .section(';', 0, 0)
                .trimmed()
                .compare(QStringLiteral("application/json"), Qt::CaseInsensitive) != 0) {
            reply(route, 415);
            return;
        }
        if (!headers.value("accept").toString().contains("application/json") ||
            !headers.value("accept").toString().contains("text/event-stream")) {
            reply(route, 406);
            return;
        }
        if (envelope.contains("parseError")) {
            reply(route,
                  400,
                  rpcError({}, envelope.value("parseError").toInt(), "Invalid JSON-RPC message."));
            return;
        }
        const auto validationError = rpcValidationError(message);
        if (!validationError.isEmpty()) {
            // 先验证消息再分配后端上下文；202 不能掩盖 wire 层拒绝。
            reply(route, 400, validationError);
            return;
        }
        if (modern && !message.contains("id")) {
            // 2026 HTTP 核心协议不定义客户端通知；取消由关闭请求流表达。
            // 通知没有 RequestMeta，也不能为其分配隐藏的客户端上下文。
            reply(route,
                  400,
                  rpcError({}, -32600, "Client notifications are not supported on 2026 HTTP."));
            return;
        }
        const auto bodyVersion =
            params.value("_meta").toObject().value("io.modelcontextprotocol/protocolVersion");
        if (!bodyVersion.isUndefined() && bodyVersion != protocol) {
            // 分流和取消均以 HTTP 代际为准，正文不得切换另一代 wire 语义。
            reply(route, 400, rpcError(message.value("id"), -32020, "HeaderMismatch"));
            return;
        }
        if (modern) {
            QString name = headers.value("mcp-name").toString();
            if (name.startsWith("=?base64?") && name.endsWith("?=")) {
                const auto decoded =
                    QByteArray::fromBase64Encoding(name.mid(9, name.size() - 11).toLatin1(),
                                                   QByteArray::AbortOnBase64DecodingErrors);
                name = decoded ? QString::fromUtf8(decoded.decoded) : QString{};
            }
            if (params.value("_meta").toObject().value("io.modelcontextprotocol/protocolVersion") !=
                    protocol ||
                headers.value("mcp-method") != rpcMethod || rpcMethod.isEmpty() ||
                (rpcMethod == "tools/call" && (name.isEmpty() || params.value("name") != name))) {
                reply(route, 400, rpcError(message.value("id"), -32020, "HeaderMismatch"));
                return;
            }
            if (!session.isEmpty() || rpcMethod == "initialize" || message.contains("result") ||
                message.contains("error")) {
                reply(route, 400, rpcError(message.value("id"), -32600, "Invalid 2026 request."));
                return;
            }
        } else if (rpcMethod != "initialize" && protocol.isEmpty()) {
            reply(route, 400, rpcError(message.value("id"), -32020, "Missing protocol version."));
            return;
        }
        if (exchanges.size() >= MaxExchanges || wires.size() >= MaxExchanges + MaxClients) {
            reply(route, 429);
            return;
        }
        QString peer;
        WireSession *wire = nullptr;
        QJsonObject responseHeaders;
        if (modern) {
            peer = "http-" + client;
            if (!attach(peer, client, true)) {
                reply(route, 429);
                return;
            }
            wire = createWire(peer);
        } else if (rpcMethod == "initialize") {
            if (!session.isEmpty()) {
                reply(route, 400);
                return;
            }
            peer = newId();
            if (!attach(peer, client, false)) {
                reply(route, 429);
                return;
            }
            wire = createWire(peer);
            contexts[peer].wire = wire;
            responseHeaders.insert("Mcp-Session-Id", peer);
        } else {
            peer = session;
            if (!contexts.contains(peer) || contexts.value(peer).modern ||
                contexts.value(peer).client != client) {
                reply(route, session.isEmpty() ? 400 : 404);
                return;
            }
            wire = contexts.value(peer).wire;
        }
        contexts[peer].touched = now();
        const bool hasId = message.contains("id");
        const bool notificationOrResponse = !hasId || rpcMethod.isEmpty();
        if (notificationOrResponse) {
            wire->receiveMessage(message, route);
            reply(route, 202);
            if (modern)
                wire->deleteLater();
            return;
        }
        const bool sse = rpcMethod == "tools/call";
        exchanges.insert(
            route,
            Exchange{peer, wire, responseHeaders, modern, sse, rpcMethod == "initialize"});
        if (sse && !io.respond(route, 200, {}, responseHeaders, true, false)) {
            dropped(route);
            return;
        }
        wire->receiveMessage(message, route);
    }
    void stop()
    {
        stopping = true;
        timer.stop();
        const auto peers = contexts.keys();
        for (const auto &peer : peers)
            close(peer);
        io.stop();
        exchanges.clear();
        jobs.clear();
    }
    HttpMcpEndpoint *q;
    Backend backend;
    HttpMcpServer io;
    QTimer timer;
    bool stopping{false};
    QHash<QString, Context> contexts;
    QHash<QString, Exchange> exchanges;
    QHash<QString, QPointer<WireSession>> jobs;
    QSet<WireSession *> wires;
};

HttpMcpEndpoint::HttpMcpEndpoint(Backend backend, QObject *parent)
    : QObject(parent), _impl(std::make_unique<Impl>(this, std::move(backend)))
{
}
HttpMcpEndpoint::~HttpMcpEndpoint()
{
    stop();
    // QObject 基类析构晚于 Impl；先切断捕获 Impl 的 child 回调。
    const auto wires = _impl->wires;
    for (auto *wire : wires) {
        QObject::disconnect(wire, nullptr, this, nullptr);
        delete wire;
    }
}
void HttpMcpEndpoint::start(quint16 port)
{
    _impl->stopping = false;
    _impl->timer.start();
    _impl->io.start(port);
}
void HttpMcpEndpoint::stop() { _impl->stop(); }
bool HttpMcpEndpoint::send(const QString &peer, const QJsonObject &message)
{
    const auto key = peer + '/' + message.value("id").toString();
    const auto wire = _impl->jobs.value(key);
    if (!wire)
        return false;
    if (message.value("op") == "result" || message.value("op") == "input_required")
        _impl->jobs.remove(key);
    wire->receiveBackend(message);
    return true;
}
void HttpMcpEndpoint::closeConnection(const QString &peer) { _impl->close(peer); }
QJsonObject HttpMcpEndpoint::statistics() const { return _impl->io.statistics(); }
} // namespace NovaTerm::Mcp
