/**
 * @file HttpMcpServer.cpp
 * @brief Qt HttpServer 的本机 I/O 线程、有界请求与异步响应队列。
 */
#include "HttpMcpServer.h"
#include "McpProtocol.h"
#include "core/ThreadNaming.h"
#include <QAbstractHttpServer>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QHttpHeaders>
#include <QHttpServerRequest>
#include <QHttpServerResponder>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>

namespace NovaTerm::Mcp
{
namespace
{
constexpr const char *IoThreadName = "nvterm-mcp-http";
constexpr qsizetype HeaderLimit = 16 * 1024;
constexpr qsizetype BodyLimit = MaxRequestBytes;
constexpr qsizetype RawRequestLimit = BodyLimit + HeaderLimit;
// HTTP 头转为 JSON 时最坏有六倍转义；同时覆盖 method/path 字段。
constexpr qsizetype EnvelopeLimit = BodyLimit + 128 * 1024;
// 单次最大请求可同时保留 guard、Qt transaction/body 与待投递 JSON 信封。
// 原始交换与信封共享额度；32个请求与16个 socket 仍各有独立数量上限。
constexpr qsizetype InputLimit = 4 * RawRequestLimit + 128 * 1024;
constexpr qsizetype OutputLimit = 4 * 1024 * 1024;
constexpr qsizetype GlobalOutputLimit = 8 * 1024 * 1024;
constexpr qsizetype SocketBuffer = 16 * 1024;
constexpr int SocketLimit = 16;
constexpr int RequestLimit = 32;
using Status = QHttpServerResponder::StatusCode;
using PeerEndpoint = std::pair<QHostAddress, quint16>;

bool token(const QByteArray &value)
{
    if (value.isEmpty())
        return false;
    for (const unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            QByteArray("!#$%&'*+-.^_`|~").contains(char(c)))
            continue;
        return false;
    }
    return true;
}

bool headerValue(const QByteArray &value)
{
    for (const unsigned char c : value)
        if ((c < 32 && c != '\t') || c == 127)
            return false;
    return true;
}

bool decimal(const QByteArray &value, quint64 &result)
{
    if (value.isEmpty())
        return false;
    result = 0;
    for (const unsigned char c : value) {
        if (c < '0' || c > '9' || result > (UINT64_MAX - (c - '0')) / 10)
            return false;
        result = result * 10 + c - '0';
    }
    return true;
}

bool allowedHost(const QByteArray &value, quint16 port)
{
    const auto host = value.toLower();
    const auto suffix = QByteArray(":") + QByteArray::number(port);
    return host == "127.0.0.1" + suffix || host == "localhost" + suffix ||
           (port == 80 && (host == "127.0.0.1" || host == "localhost"));
}

struct Credit {
    qsizetype pending{0};
    bool started{false};
    bool sse{false};
    bool final{false};
    bool abort{false};
    bool gone{false};
};
struct Event {
    enum class Kind
    {
        Listening,
        Request,
        Disconnected
    } kind;
    QString id;
    QJsonObject envelope;
    qsizetype charge{0};
    bool ok{false};
};
struct Response {
    QString id;
    int status;
    QByteArray body;
    QHttpHeaders headers;
    qsizetype charge;
    bool first;
    bool sse;
    bool final;
};
struct Queues {
    std::mutex mutex;
    std::deque<Event> incoming;
    std::deque<Response> outgoing;
    QHash<QString, Credit> credits;
    qsizetype inputBytes{0};
    qsizetype outputBytes{0};
    int liveSockets{0};
    int peakSockets{0};
    quint64 acceptedRequests{0};
    quint64 rejectedRequests{0};
    bool guiScheduled{false};
    bool ioScheduled{false};
    bool stopping{true};
    quint64 generation{0};
    QObject *worker{nullptr};
};

/** @brief 在交给 Qt parser 前暂存并核验原始交换，读缓冲与 socket 同属 I/O 线程。 */
class GuardSocket final : public QTcpSocket
{
public:
    explicit GuardSocket(QObject *parent) : QTcpSocket(parent) {}
    QByteArray raw;
    qsizetype cursor{0};
    bool replay{false};
    qint64 networkAvailable() const { return QTcpSocket::bytesAvailable(); }
    qint64 readNetwork(char *bytes, qint64 count) { return QTcpSocket::read(bytes, count); }
    qint64 bytesAvailable() const override
    {
        return replay ? raw.size() - cursor + QIODevice::bytesAvailable()
                      : QTcpSocket::bytesAvailable();
    }

protected:
    qint64 readData(char *bytes, qint64 count) override
    {
        // 保留 QAbstractSocket 的读通知恢复语义；原始网络字节仍走其有界缓冲。
        QTcpSocket::readData(nullptr, 0);
        if (!replay)
            return 0;
        const auto size = std::min<qint64>(count, raw.size() - cursor);
        if (size > 0)
            std::memcpy(bytes, raw.constData() + cursor, size_t(size));
        cursor += qsizetype(size);
        return size;
    }
};

class HttpWorker;
class AcceptServer final : public QTcpServer
{
public:
    explicit AcceptServer(HttpWorker *worker);
    void handOver(QTcpSocket *socket)
    {
        addPendingConnection(socket);
        emit newConnection();
    }

protected:
    void incomingConnection(qintptr descriptor) override;

private:
    HttpWorker *_worker;
};

class HttpWorker final : public QAbstractHttpServer
{
public:
    HttpWorker(std::shared_ptr<Queues> queues, HttpMcpServer *owner)
        : _queues(std::move(queues)), _owner(owner), _server(new AcceptServer(this))
    {
        _clock.start();
        _server->setMaxPendingConnections(SocketLimit);
        _timer.setInterval(std::chrono::milliseconds{1000});
        connect(&_timer, &QTimer::timeout, this, [this] {
            const auto peers = _peers.values();
            for (const auto &peer : peers) {
                if (peer->socket &&
                    ((!peer->accepted && _clock.elapsed() - peer->created > 10000) ||
                     (peer->socket->bytesToWrite() && _clock.elapsed() - peer->progress > 10000)))
                    peer->socket->abort();
            }
        });
        _timer.start();
    }
    ~HttpWorker() override
    {
        _shuttingDown = true;
        _server->close();
        // responder 的析构会回调 Qt 的协议处理器，必须在 socket 销毁前完成。
        const auto peers = _peers.values();
        for (const auto &peer : peers)
            peer->responder.reset();
        for (const auto &peer : peers)
            if (peer->socket)
                peer->socket->abort();
        _peers.clear();
    }
    void start(quint16 port)
    {
        const bool ok = _server->listen(QHostAddress::LocalHost, port) && bind(_server);
        enqueue(
            {Event::Kind::Listening,
             ok ? QStringLiteral("http://127.0.0.1:%1/mcp").arg(_server->serverPort()) : QString(),
             {},
             0,
             ok});
    }
    void accept(qintptr descriptor)
    {
        auto *socket = new GuardSocket(this);
        if (!socket->setSocketDescriptor(descriptor)) {
            delete socket;
            return;
        }
        socket->setReadBufferSize(SocketBuffer);
        if (_peers.size() >= SocketLimit) {
            socket->abort();
            socket->deleteLater();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(_queues->mutex);
            ++_queues->liveSockets;
            _queues->peakSockets = (std::max)(_queues->peakSockets, _queues->liveSockets);
        }
        connect(socket, &QObject::destroyed, this, [queues = _queues] {
            std::lock_guard<std::mutex> lock(queues->mutex);
            --queues->liveSockets;
        });
        auto peer = std::make_shared<Peer>();
        peer->socket = socket;
        peer->endpoint = {socket->peerAddress(), socket->peerPort()};
        peer->created = peer->progress = _clock.elapsed();
        _peers.insert(peer->endpoint, peer);
        // 此槽先于 Qt handler 的 readyRead 注册，在解析前核算新到字节。
        connect(socket, &QTcpSocket::readyRead, this, [this, peer] { guard(peer); });
        connect(socket, &QTcpSocket::bytesWritten, this, [this, peer](qint64 amount) {
            peer->progress = _clock.elapsed();
            std::lock_guard<std::mutex> lock(_queues->mutex);
            auto it = _queues->credits.find(peer->id);
            if (it == _queues->credits.end())
                return;
            const auto released = std::min<qsizetype>(peer->writtenCredit, qsizetype(amount));
            peer->writtenCredit -= released;
            it->pending -= released;
            _queues->outputBytes -= released;
        });
        connect(socket, &QTcpSocket::disconnected, this, [this, peer] { closed(peer); });
    }
    void flush()
    {
        std::deque<Response> responses;
        QStringList aborts;
        {
            std::lock_guard<std::mutex> lock(_queues->mutex);
            responses.swap(_queues->outgoing);
            _queues->ioScheduled = false;
            for (auto it = _queues->credits.cbegin(); it != _queues->credits.cend(); ++it)
                if (it->abort && !it->gone)
                    aborts.push_back(it.key());
        }
        for (const auto &id : aborts) {
            auto peer = find(id);
            if (peer && peer->socket)
                peer->socket->abort();
        }
        for (const auto &response : responses) {
            auto peer = find(response.id);
            if (!peer || !peer->socket || !peer->responder)
                continue;
            QByteArray body = response.body;
            if (response.sse && !body.isEmpty()) {
                // JSON 校验和 SSE 组帧都在 I/O 线程，保留已编码 RPC 的字节。
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(body, &error);
                if (error.error != QJsonParseError::NoError || !document.isObject()) {
                    peer->socket->abort();
                    continue;
                }
                body.replace("\r\n", "\n");
                body.replace('\r', '\n');
                body.replace("\n", "\ndata:");
                body.prepend("data:");
                body.append("\n\n");
            }
            auto &responder = *peer->responder;
            const auto before = peer->socket->bytesToWrite();
            if (response.sse) {
                if (response.first)
                    responder.writeBeginChunked(response.headers, Status(response.status));
                if (response.final) {
                    // Qt 6.8 的 writeEndChunked 空正文会告警；给最终事件一次性写出。
                    // 空终止使用一个 SSE 注释，既不产生业务事件也避免 Qt 告警。
                    responder.writeEndChunked(body.isEmpty() ? QByteArray(":\n\n") : body);
                } else if (!body.isEmpty())
                    responder.writeChunk(body);
            } else
                responder.write(response.body, response.headers, Status(response.status));
            const auto actual = qsizetype(peer->socket->bytesToWrite() - before);
            {
                std::lock_guard<std::mutex> lock(_queues->mutex);
                auto it = _queues->credits.find(peer->id);
                if (it != _queues->credits.end()) {
                    const auto unused = response.charge - actual;
                    it->pending -= unused;
                    _queues->outputBytes -= unused;
                    peer->writtenCredit += actual;
                }
            }
            if (response.final) {
                peer->responder.reset();
                peer->socket->disconnectFromHost();
            }
        }
    }

private:
    struct Peer {
        QPointer<GuardSocket> socket;
        std::unique_ptr<QHttpServerResponder> responder;
        QString id;
        PeerEndpoint endpoint;
        qsizetype rawBytes{0};
        qsizetype chargedRaw{0};
        qsizetype headerEnd{0};
        qsizetype expectationOffset{0};
        qsizetype expectationLength{0};
        QByteArray expectation;
        qsizetype scan{0};
        qsizetype chunkRemaining{0};
        qsizetype decodedBytes{0};
        qint64 contentLength{0};
        enum class ChunkState
        {
            Size,
            Data,
            Terminator,
            LastTerminator,
            Done
        };
        ChunkState chunkState{ChunkState::Size};
        bool chunked{false};
        bool handedOver{false};
        qsizetype writtenCredit{0};
        qint64 created{0};
        qint64 progress{0};
        bool headersDone{false};
        bool accepted{false};
        bool rejected{false};
    };
    std::shared_ptr<Peer> find(const QString &id) const
    {
        for (const auto &peer : _peers)
            if (peer->id == id)
                return peer;
        return {};
    }
    void enqueue(Event event)
    {
        bool wake = false;
        {
            std::lock_guard<std::mutex> lock(_queues->mutex);
            if (_queues->stopping)
                return;
            _queues->incoming.push_back(std::move(event));
            if (!_queues->guiScheduled) {
                _queues->guiScheduled = true;
                wake = true;
            }
        }
        if (wake)
            QMetaObject::invokeMethod(_owner, "drain", Qt::QueuedConnection);
    }
    void
    reject(const std::shared_ptr<Peer> &peer, int status, QHttpServerResponder *responder = nullptr)
    {
        if (!peer->socket || peer->rejected)
            return;
        peer->rejected = true;
        {
            std::lock_guard<std::mutex> lock(_queues->mutex);
            ++_queues->rejectedRequests;
        }
        // 拒绝未完整解析的交换，禁止 Qt handler 再消费剩余输入。
        peer->socket->disconnect(peer->socket, &QTcpSocket::readyRead, nullptr, nullptr);
        if (responder) {
            // Qt responder 若在 ClosingState 析构会提前销毁 handler/socket，丢弃待写头。
            // 已解析交换用正式 responder，并保留到 closed()，保证拒绝响应排空。
            peer->responder = std::make_unique<QHttpServerResponder>(std::move(*responder));
            QHttpHeaders headers;
            headers.append("Connection", "close");
            peer->responder->write(QByteArray{}, headers, Status(status));
        } else {
            peer->socket->write("HTTP/1.1 " + QByteArray::number(status) +
                                " Rejected\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
        }
        peer->socket->disconnectFromHost();
    }
    int validatePrefix(const QByteArray &prefix, quint16 port) const
    {
        const auto lines = prefix.split('\n');
        if (lines.isEmpty() || !lines.first().endsWith(" HTTP/1.1\r"))
            return 400;
        QHash<QByteArray, QByteArray> fields;
        for (qsizetype i = 1; i < lines.size(); ++i) {
            auto line = lines.at(i);
            if (line.isEmpty() || line == "\r")
                continue;
            if (!line.endsWith('\r') || line.startsWith(' ') || line.startsWith('\t'))
                return 400;
            line.chop(1);
            const auto colon = line.indexOf(':');
            if (colon <= 0 || !token(line.left(colon)))
                return 400;
            const auto name = line.left(colon).toLower();
            const auto value = line.mid(colon + 1).trimmed();
            if (!headerValue(value) || fields.contains(name))
                return 400;
            fields.insert(name, value);
        }
        if (!allowedHost(fields.value("host"), port))
            return 400;
        if (fields.contains("origin")) {
            const auto origin = fields.value("origin").toLower();
            if (!origin.startsWith("http://") || !allowedHost(origin.mid(7), port))
                return 403;
        }
        if (fields.contains("content-length")) {
            quint64 length = 0;
            if (fields.contains("transfer-encoding") ||
                !decimal(fields.value("content-length"), length))
                return 400;
            if (length > quint64(BodyLimit))
                return 413;
        }
        if (fields.contains("transfer-encoding") &&
            fields.value("transfer-encoding").toLower() != "chunked")
            return 400;
        if (fields.contains("upgrade") || fields.contains("trailer"))
            return 400;
        if (fields.contains("expect") && fields.value("expect").toLower() != "100-continue")
            return 417;
        return 0;
    }
    int chunkGuard(const std::shared_ptr<Peer> &peer)
    {
        const auto &raw = peer->socket->raw;
        while (true) {
            switch (peer->chunkState) {
            case Peer::ChunkState::Size: {
                const auto end = raw.indexOf("\r\n", peer->scan);
                if (end < 0)
                    return raw.size() - peer->scan > 1024 ? 400 : 0;
                auto line = raw.mid(peer->scan, end - peer->scan);
                const auto semicolon = line.indexOf(';');
                // 扩展不影响边界；禁止控制符以及空扩展，避免 Qt 宽松换行归一化。
                if (semicolon >= 0) {
                    if (!headerValue(line.mid(semicolon + 1)) || semicolon + 1 == line.size())
                        return 400;
                    line.truncate(semicolon);
                }
                if (line.isEmpty())
                    return 400;
                quint64 size = 0;
                for (const unsigned char c : line) {
                    int digit = c >= '0' && c <= '9'   ? c - '0'
                                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                       : -1;
                    if (digit < 0 || size > (UINT64_MAX - quint64(digit)) / 16)
                        return 400;
                    size = size * 16 + quint64(digit);
                }
                if (size > quint64(BodyLimit - peer->decodedBytes))
                    return 413;
                peer->decodedBytes += qsizetype(size);
                peer->chunkRemaining = qsizetype(size);
                peer->scan = end + 2;
                peer->chunkState = size ? Peer::ChunkState::Data : Peer::ChunkState::LastTerminator;
                break;
            }
            case Peer::ChunkState::Data: {
                const auto count = (std::min)(peer->chunkRemaining, raw.size() - peer->scan);
                peer->scan += count;
                peer->chunkRemaining -= count;
                if (peer->chunkRemaining)
                    return 0;
                peer->chunkState = Peer::ChunkState::Terminator;
                break;
            }
            case Peer::ChunkState::Terminator:
            case Peer::ChunkState::LastTerminator:
                if (raw.size() - peer->scan < 2)
                    return 0;
                if (raw.mid(peer->scan, 2) != "\r\n")
                    return 400;
                peer->scan += 2;
                peer->chunkState = peer->chunkState == Peer::ChunkState::LastTerminator
                                       ? Peer::ChunkState::Done
                                       : Peer::ChunkState::Size;
                break;
            case Peer::ChunkState::Done:
                return raw.size() == peer->scan ? 0 : 400;
            }
        }
    }
    void guard(const std::shared_ptr<Peer> &peer)
    {
        if (!peer->socket || peer->rejected)
            return;
        if (peer->handedOver) {
            // 每条 TCP 只允许一次交换；新到数据不能成为第二个业务请求。
            if (peer->socket->networkAvailable())
                peer->socket->abort();
            return;
        }
        char buffer[SocketBuffer];
        while (peer->socket->networkAvailable() > 0) {
            const auto wanted = std::min<qint64>(SocketBuffer, peer->socket->networkAvailable());
            bool overloaded = false;
            {
                std::lock_guard<std::mutex> lock(_queues->mutex);
                // Qt 解析期间会同时保留 transaction、body 与原始 guard 缓冲。
                // 按三份预留；解析提交后退还一份，再入队不可变 envelope。
                overloaded = _queues->stopping || _queues->inputBytes + wanted * 3 > InputLimit;
                if (!overloaded) {
                    _queues->inputBytes += wanted * 3;
                    peer->chargedRaw += wanted * 3;
                }
            }
            if (overloaded) {
                reject(peer, 503);
                return;
            }
            const auto count = peer->socket->readNetwork(buffer, wanted);
            if (count <= 0) {
                peer->socket->abort();
                return;
            }
            if (count < wanted) {
                std::lock_guard<std::mutex> lock(_queues->mutex);
                const auto refund = qsizetype(wanted - count) * 3;
                _queues->inputBytes -= refund;
                peer->chargedRaw -= refund;
            }
            peer->rawBytes += qsizetype(count);
            peer->socket->raw.append(buffer, qsizetype(count));
            if (peer->rawBytes > RawRequestLimit) {
                reject(peer, 413);
                return;
            }
            if (!peer->headersDone) {
                const auto end = peer->socket->raw.indexOf("\r\n\r\n");
                if (end < 0) {
                    if (peer->rawBytes > HeaderLimit) {
                        reject(peer, 431);
                        return;
                    }
                    continue;
                }
                peer->headerEnd = end + 4;
                if (peer->headerEnd > HeaderLimit) {
                    reject(peer, 431);
                    return;
                }
                const auto prefix = peer->socket->raw.left(peer->headerEnd);
                const int error = validatePrefix(prefix, _server->serverPort());
                if (error) {
                    reject(peer, error);
                    return;
                }
                const auto fields = prefix.split('\n');
                qsizetype offset = fields.first().size() + 1;
                for (qsizetype i = 1; i < fields.size(); ++i) {
                    const auto &field = fields.at(i);
                    const auto lower = field.toLower();
                    if (lower.startsWith("transfer-encoding:"))
                        peer->chunked = true;
                    if (lower.startsWith("content-length:")) {
                        quint64 length = 0;
                        decimal(field.mid(field.indexOf(':') + 1).trimmed(), length);
                        peer->contentLength = qint64(length);
                    }
                    if (lower.startsWith("expect:")) {
                        peer->expectationOffset = offset;
                        peer->expectationLength = field.size() + 1;
                        peer->expectation = field.mid(field.indexOf(':') + 1).trimmed();
                    }
                    offset += field.size() + 1;
                }
                peer->headersDone = true;
                peer->scan = peer->headerEnd;
                // Host/Origin、长度与 framing 全部核验后才准许发送正文。
                // 每条连接最多写一次固定大小的临时响应；正文仍由 guard 有界暂存。
                if (!peer->expectation.isEmpty() && (peer->chunked || peer->contentLength > 0))
                    peer->socket->write("HTTP/1.1 100 Continue\r\n\r\n");
            }
            bool complete = false;
            if (peer->chunked) {
                const int error = chunkGuard(peer);
                if (error) {
                    reject(peer, error);
                    return;
                }
                complete = peer->chunkState == Peer::ChunkState::Done;
            } else {
                const auto length = peer->headerEnd + peer->contentLength;
                if (peer->rawBytes > length) {
                    reject(peer, 400);
                    return;
                }
                complete = peer->rawBytes == length;
            }
            if (complete) {
                // Qt 6.8 会自行处理 Expect；回放时移除已履行的头，防止重复 100。
                // rawBytes/chargedRaw 保留原始字节额度，三份预算继续覆盖解析副本。
                if (peer->expectationLength)
                    peer->socket->raw.remove(peer->expectationOffset, peer->expectationLength);
                peer->handedOver = true;
                peer->socket->replay = true;
                _server->handOver(peer->socket);
                // Qt handler 已通过公共 newConnection 注册，此次通知解析完整输入。
                QMetaObject::invokeMethod(peer->socket, "readyRead", Qt::QueuedConnection);
                return;
            }
        }
    }
    void closed(const std::shared_ptr<Peer> &peer)
    {
        if (_shuttingDown)
            return;
        // 源端口不是连接身份；不同回环 IP 可以使用同一源端口。
        // 即使旧连接迟到断开，也只能移除它自己占有的 endpoint 条目。
        if (_peers.value(peer->endpoint) == peer)
            _peers.remove(peer->endpoint);
        {
            std::lock_guard<std::mutex> lock(_queues->mutex);
            _queues->inputBytes -= peer->chargedRaw;
            auto it = _queues->credits.find(peer->id);
            if (it != _queues->credits.end()) {
                _queues->outputBytes -= it->pending;
                it->pending = 0;
                it->gone = true;
            }
        }
        peer->responder.reset();
        // 未交给 Qt 协议处理器的拒绝/半包 socket 仍归 worker 所有。
        // 断连即回收，避免保留它的原始缓冲与捕获 peer 的信号槽。
        if (!peer->handedOver && peer->socket)
            peer->socket->deleteLater();
        if (peer->accepted)
            enqueue({Event::Kind::Disconnected, peer->id, {}, 0, false});
    }
    bool handleRequest(const QHttpServerRequest &request, QHttpServerResponder &responder) override
    {
        const auto peer = _peers.value({request.remoteAddress(), request.remotePort()});
        if (!peer || !peer->socket || peer->rejected)
            return true;
        if (peer->accepted || !peer->headersDone) {
            reject(peer, 400, &responder);
            return true;
        }
        {
            std::lock_guard<std::mutex> lock(_queues->mutex);
            _queues->inputBytes -= peer->rawBytes;
            peer->chargedRaw -= peer->rawBytes;
        }
        peer->socket->raw.clear();
        peer->socket->cursor = 0;
        if (request.body().size() > BodyLimit) {
            reject(peer, 413, &responder);
            return true;
        }
        if (request.url().path() != QStringLiteral("/mcp") || request.url().hasQuery()) {
            reject(peer, 404, &responder);
            return true;
        }
        QString method;
        switch (request.method()) {
        case QHttpServerRequest::Method::Post:
            method = QStringLiteral("POST");
            break;
        case QHttpServerRequest::Method::Get:
            method = QStringLiteral("GET");
            break;
        case QHttpServerRequest::Method::Delete:
            method = QStringLiteral("DELETE");
            break;
        default:
            reject(peer, 405, &responder);
            return true;
        }
        QJsonObject headers;
        qsizetype headerBytes = 0;
        for (const auto &pair : request.headers().toListOfPairs()) {
            headerBytes += pair.first.size() + pair.second.size() + 4;
            headers.insert(QString::fromLatin1(pair.first.toLower()),
                           QString::fromLatin1(pair.second));
        }
        if (!peer->expectation.isEmpty()) {
            headerBytes += peer->expectationLength;
            headers.insert(QStringLiteral("expect"), QString::fromLatin1(peer->expectation));
        }
        if (headerBytes > HeaderLimit) {
            reject(peer, 431, &responder);
            return true;
        }
        QJsonObject envelope{{"method", method}, {"path", "/mcp"}, {"headers", headers}};
        if (method == QStringLiteral("POST")) {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(request.body(), &error);
            if (error.error != QJsonParseError::NoError)
                envelope.insert("parseError", -32700);
            else if (!document.isObject())
                envelope.insert("parseError", -32600);
            else
                envelope.insert("message", document.object());
        }
        const auto charge = QJsonDocument(envelope).toJson(QJsonDocument::Compact).size();
        bool admitted = false;
        {
            std::lock_guard<std::mutex> lock(_queues->mutex);
            admitted = !_queues->stopping && _queues->credits.size() < RequestLimit &&
                       charge <= EnvelopeLimit && _queues->inputBytes + charge <= InputLimit;
            if (admitted) {
                peer->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                _queues->credits.insert(peer->id, {});
                _queues->inputBytes += charge;
                ++_queues->acceptedRequests;
            }
        }
        if (!admitted) {
            reject(peer, 503, &responder);
            return true;
        }
        peer->accepted = true;
        peer->responder = std::make_unique<QHttpServerResponder>(std::move(responder));
        enqueue({Event::Kind::Request, peer->id, std::move(envelope), charge, false});
        return true;
    }
    void missingHandler(const QHttpServerRequest &, QHttpServerResponder &responder) override
    {
        responder.write(Status::NotFound);
    }
    std::shared_ptr<Queues> _queues;
    HttpMcpServer *_owner;
    AcceptServer *_server;
    QHash<PeerEndpoint, std::shared_ptr<Peer>> _peers;
    QElapsedTimer _clock;
    QTimer _timer;
    bool _shuttingDown{false};
};
AcceptServer::AcceptServer(HttpWorker *worker) : QTcpServer(worker), _worker(worker) {}
void AcceptServer::incomingConnection(qintptr descriptor) { _worker->accept(descriptor); }

class IoThread final : public QThread
{
public:
    std::shared_ptr<Queues> queues;
    HttpMcpServer *owner{nullptr};
    quint16 port{0};
    void run() override
    {
        NovaTerm::setCurrentThreadName(IoThreadName);
        HttpWorker worker(queues, owner);
        {
            std::lock_guard<std::mutex> lock(queues->mutex);
            queues->worker = &worker;
        }
        worker.start(port);
        exec();
        std::lock_guard<std::mutex> lock(queues->mutex);
        queues->worker = nullptr;
    }
};
} // namespace

class HttpMcpServer::Impl
{
public:
    std::shared_ptr<Queues> queues{std::make_shared<Queues>()};
    IoThread thread;
};
HttpMcpServer::HttpMcpServer(QObject *parent) : QObject(parent), _impl(std::make_unique<Impl>()) {}
HttpMcpServer::~HttpMcpServer() { stop(); }
QJsonObject HttpMcpServer::statistics() const
{
    std::lock_guard<std::mutex> lock(_impl->queues->mutex);
    const auto &queues = *_impl->queues;
    return {{"liveSockets", queues.liveSockets},
            {"peakSockets", queues.peakSockets},
            {"inputBytes", double(queues.inputBytes)},
            {"outputBytes", double(queues.outputBytes)},
            {"pendingRequests", queues.credits.size()},
            {"incomingEvents", int(queues.incoming.size())},
            {"outgoingEvents", int(queues.outgoing.size())},
            {"acceptedRequests", QString::number(queues.acceptedRequests)},
            {"rejectedRequests", QString::number(queues.rejectedRequests)}};
}
void HttpMcpServer::start(quint16 port)
{
    if (_impl->thread.isRunning())
        return;
    {
        std::lock_guard<std::mutex> lock(_impl->queues->mutex);
        _impl->queues->stopping = false;
        ++_impl->queues->generation;
    }
    _impl->thread.queues = _impl->queues;
    _impl->thread.owner = this;
    _impl->thread.port = port;
    _impl->thread.setObjectName(QString::fromLatin1(IoThreadName));
    _impl->thread.start();
}
void HttpMcpServer::stop()
{
    {
        std::lock_guard<std::mutex> lock(_impl->queues->mutex);
        _impl->queues->stopping = true;
    }
    _impl->thread.quit();
    _impl->thread.wait();
    std::lock_guard<std::mutex> lock(_impl->queues->mutex);
    _impl->queues->incoming.clear();
    _impl->queues->outgoing.clear();
    _impl->queues->credits.clear();
    _impl->queues->inputBytes = _impl->queues->outputBytes = 0;
    // worker 的 QObject 析构会先断开 destroyed 槽；线程退出后已无存活 socket。
    // 仅归零当前值，历史峰值仍可跨 stop/start 用于诊断。
    _impl->queues->liveSockets = 0;
    _impl->queues->ioScheduled = false;
}
bool HttpMcpServer::respond(
    QString id, int status, QByteArray body, QJsonObject headers, bool sse, bool final)
{
    if (status < 200 || status > 599 || (!sse && !final) || (sse && status != 200) ||
        ((status == 204 || status == 304) && !body.isEmpty()))
        return false;
    QHttpHeaders wireHeaders;
    qsizetype headerBytes = 0;
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        const auto name = it.key().toLatin1();
        const auto value = it.value().toString().toUtf8();
        const auto lower = name.toLower();
        if (!it.value().isString() || !token(name) || !headerValue(value) ||
            lower == "content-length" || lower == "transfer-encoding" || lower == "connection" ||
            lower == "content-type" || wireHeaders.contains(name))
            return false;
        headerBytes += name.size() + value.size() + 4;
        if (headerBytes > HeaderLimit || !wireHeaders.append(name, value))
            return false;
    }
    wireHeaders.append("Connection", "close");
    wireHeaders.append("Content-Type", sse ? "text/event-stream" : "application/json");
    // 覆盖 Qt 状态行、自动头、chunk 边界与最终空块；实际写出后退还差额。
    // 非紧凑 JSON 的多行 data 前缀按上界预留；实际写出时退还差额。
    const qsizetype sseBytes =
        sse && !body.isEmpty() ? 7 + 5 * (body.count('\n') + body.count('\r')) : 0;
    const qsizetype charge = body.size() + sseBytes + headerBytes + 512;
    const auto queues = _impl->queues;
    std::lock_guard<std::mutex> lock(queues->mutex);
    auto it = queues->credits.find(id);
    if (queues->stopping || !queues->worker || it == queues->credits.end() || it->gone ||
        it->final || it->abort || (it->started && (it->sse != sse || !headers.isEmpty())))
        return false;
    const bool overflow = charge > OutputLimit - it->pending ||
                          charge > GlobalOutputLimit - queues->outputBytes ||
                          queues->outgoing.size() >= 64;
    if (overflow)
        it->abort = true;
    else {
        queues->outgoing.push_back({id,
                                    status,
                                    std::move(body),
                                    std::move(wireHeaders),
                                    charge,
                                    !it->started,
                                    sse,
                                    final});
        queues->outputBytes += charge;
        it->pending += charge;
        it->started = true;
        it->sse = sse;
        it->final = final;
    }
    if (!queues->ioScheduled) {
        queues->ioScheduled = true;
        auto *worker = static_cast<HttpWorker *>(queues->worker);
        // 在队列锁内投递，stop 不会在此处与 worker 的析构交错。
        QMetaObject::invokeMethod(worker, [worker] { worker->flush(); }, Qt::QueuedConnection);
    }
    return !overflow;
}
void HttpMcpServer::drain()
{
    std::deque<Event> events;
    quint64 generation = 0;
    {
        std::lock_guard<std::mutex> lock(_impl->queues->mutex);
        generation = _impl->queues->generation;
        events.swap(_impl->queues->incoming);
        _impl->queues->guiScheduled = false;
        for (const auto &event : events)
            _impl->queues->inputBytes -= event.charge;
    }
    for (const auto &event : events) {
        {
            std::lock_guard<std::mutex> lock(_impl->queues->mutex);
            if (_impl->queues->stopping || _impl->queues->generation != generation)
                return;
        }
        if (event.kind == Event::Kind::Listening)
            emit listening(event.ok, event.id);
        else if (event.kind == Event::Kind::Request)
            emit request(event.id, event.envelope);
        else {
            {
                std::lock_guard<std::mutex> lock(_impl->queues->mutex);
                _impl->queues->credits.remove(event.id);
            }
            emit disconnected(event.id);
        }
    }
}
} // namespace NovaTerm::Mcp
