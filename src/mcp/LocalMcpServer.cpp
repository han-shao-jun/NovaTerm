/** @file LocalMcpServer.cpp
 *  @brief 当前用户 IPC、分帧与慢客户端回收；不接触 Session 对象。
 */
#include "LocalMcpServer.h"
#include "McpProtocol.h"
#include "core/ThreadNaming.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonDocument>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <deque>
#include <mutex>

namespace NovaTerm::Mcp {
namespace {
constexpr const char* IoThreadName = "nvterm-mcp-io";
struct Incoming { QString id; QJsonObject message; bool disconnected{false}; };
struct Outgoing { QString id; QByteArray bytes; bool close{false}; bool authenticated{false}; };
struct Queues {
    std::mutex mutex;
    std::deque<Incoming> incoming;
    std::deque<Outgoing> outgoing;
    QHash<QString, qsizetype> pendingBytes;
    QSet<QString> authenticatedIds;
    qsizetype incomingBytes{0};
    bool guiScheduled{false};
    bool ioScheduled{false};
    bool stopping{false};
};

class IoWorker final : public QObject
{
    Q_OBJECT
public:
    IoWorker(std::shared_ptr<Queues> queues, LocalMcpServer* owner)
        : _queues(std::move(queues)), _owner(owner) {}
    void start(const QString& endpoint)
    {
        NovaTerm::setCurrentThreadName(IoThreadName);
        _clock.start();
        _server = new QLocalServer(this);
        _server->setSocketOptions(QLocalServer::UserAccessOption);
        _server->setMaxPendingConnections(MaxClients);
        connect(_server, &QLocalServer::newConnection, this, [this] { accept(); });
        const bool ok = _server->listen(endpoint);
        emit ready(ok);
    }
    void flush()
    {
        std::deque<Outgoing> outgoing;
        {
            std::lock_guard<std::mutex> lock(_queues->mutex);
            outgoing.swap(_queues->outgoing);
            _queues->ioScheduled = false;
        }
        for (auto& message : outgoing) {
            auto peer = _peers.value(message.id);
            if (!peer)
                continue;
            if (message.close) { peer->socket->abort(); continue; }
            if (message.authenticated)
                peer->authenticated = true;
            if (peer->socket->write(message.bytes) != message.bytes.size()) {
                peer->socket->abort();
            } else if (!peer->timer->isActive()) {
                peer->lastProgress = _clock.elapsed();
                peer->timer->start(1000);
            }
        }
    }
signals:
    void ready(bool ok);
private:
    struct Peer {
        QString id;
        QLocalSocket* socket{nullptr};
        QTimer* timer{nullptr};
        FrameReader frames;
        bool authenticated{false};
        qint64 acceptedAt{0};
        qint64 lastProgress{0};
    };
    bool enqueue(Incoming message)
    {
        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(_queues->mutex);
            const auto size = message.message.isEmpty() ? 0
                : QJsonDocument(message.message).toJson(QJsonDocument::Compact).size();
            if (_queues->stopping || (!message.disconnected
                && (_queues->incoming.size() >= 16 || _queues->incomingBytes + size > MaxFrameBytes)))
                return false;
            _queues->incomingBytes += size;
            _queues->incoming.push_back(std::move(message));
            if (!_queues->guiScheduled) {
                _queues->guiScheduled = true;
                notify = true;
            }
        }
        if (notify)
            QMetaObject::invokeMethod(_owner, "drain", Qt::QueuedConnection);
        return true;
    }
    void accept()
    {
        while (_server->hasPendingConnections()) {
            auto* socket = _server->nextPendingConnection();
            if (_peers.size() >= MaxClients) { socket->abort(); socket->deleteLater(); continue; }
            auto peer = std::make_shared<Peer>();
            peer->id = newId();
            // QLocalServer::nextPendingConnection() 返回**无 parent** 的 socket，
            // Qt 的契约是「调用方负责 delete」。设为 IoWorker 的子对象后，socket
            // 绝不会比 worker 活得久 —— 否则 stop() 销毁 IoWorker 时，仍连接的
            // 每个 peer 都会连同它的 QTimer 与 fd/句柄一起泄漏（每次禁用再启用
            // MCP 接入都会发生，不只是进程退出）。
            socket->setParent(this);
            peer->socket = socket;
            peer->acceptedAt = peer->lastProgress = _clock.elapsed();
            peer->timer = new QTimer(socket);
            socket->setReadBufferSize(MaxFrameBytes + 4);
            _peers.insert(peer->id, peer);
            {
                std::lock_guard<std::mutex> lock(_queues->mutex);
                _queues->pendingBytes.insert(peer->id, 0);
            }
            connect(socket, &QLocalSocket::readyRead, this, [this, peer] { read(peer); });
            connect(socket, &QLocalSocket::bytesWritten, this, [this, peer](qint64 bytes) {
                std::lock_guard<std::mutex> lock(_queues->mutex);
                auto it = _queues->pendingBytes.find(peer->id);
                if (it != _queues->pendingBytes.end())
                    *it = std::max(qsizetype(0), *it - qsizetype(bytes));
                peer->lastProgress = _clock.elapsed();
                if (peer->authenticated && socketPending(peer->id) == 0)
                    peer->timer->stop();
            });
            connect(peer->timer, &QTimer::timeout, this, [this, peer] {
                const auto now = _clock.elapsed();
                if ((!peer->authenticated && now - peer->acceptedAt > 3000)
                    || now - peer->lastProgress > 10000)
                    peer->socket->abort();
            });
            connect(socket, &QLocalSocket::disconnected, this, [this, peer] {
                if (_shuttingDown)
                    return;
                peer->timer->stop();
                _peers.remove(peer->id);
                bool authenticated = false;
                {
                    std::lock_guard<std::mutex> lock(_queues->mutex);
                    authenticated = _queues->authenticatedIds.contains(peer->id);
                    _queues->pendingBytes.remove(peer->id);
                }
                // 只有至多四个已认证连接需要通知业务层；未认证连接洪泛不能积压事件。
                if (authenticated) enqueue({peer->id, {}, true});
                peer->socket->deleteLater();
            });
            peer->timer->start(1000);
            read(peer);
        }
    }
    // 调用方已经持有队列锁。
    qsizetype socketPending(const QString& id) const { return _queues->pendingBytes.value(id); }
    void read(const std::shared_ptr<Peer>& peer)
    {
        while (peer->socket->bytesAvailable() > 0) {
            const auto amount = std::min<qsizetype>(peer->frames.capacity(), peer->socket->bytesAvailable());
            if (amount <= 0 || !peer->frames.append(peer->socket->read(amount))) {
                peer->socket->abort();
                return;
            }
            while (auto object = peer->frames.take()) {
                if (!enqueue({peer->id, std::move(*object), false})) {
                    // 不给超额请求再生成无界错误队列，断开后客户端得到明确连接失败。
                    peer->socket->abort();
                    return;
                }
            }
            if (peer->frames.failed()) { peer->socket->abort(); return; }
        }
    }
    std::shared_ptr<Queues> _queues;
    LocalMcpServer* _owner;
    QLocalServer* _server{nullptr};
    ~IoWorker()
    {
        // IoWorker 销毁时（stop() 里的 thread.finished → deleteLater，或析构）
        // 仍连接的 socket 不会触发 disconnected。socket 已是本对象的子对象，
        // ~QObject 会 delete 它们；先 abort() 只是为了让对端立刻看到断开而不是
        // 等超时。_shuttingDown 让 abort() 同步触发的 disconnected 处理器不再
        // 回头改 _peers（析构进行中）。
        _shuttingDown = true;
        const auto peers = _peers;
        _peers.clear();
        for (const auto& peer : peers) {
            if (peer->socket == nullptr)
                continue;
            peer->timer->stop();
            peer->socket->abort();
        }
    }

    // 析构收尾期间为 true：此时 abort() 同步触发的 disconnected 处理器不再
    // 回头改 _peers。
    bool _shuttingDown{false};
    QHash<QString, std::shared_ptr<Peer>> _peers;
    QElapsedTimer _clock;
};
}

class LocalMcpServer::Impl
{
public:
    std::shared_ptr<Queues> queues{std::make_shared<Queues>()};
    QThread thread;
    IoWorker* worker{nullptr};
};

LocalMcpServer::LocalMcpServer(QObject* parent) : QObject(parent), _impl(std::make_unique<Impl>()) {}
LocalMcpServer::~LocalMcpServer() { stop(); }

void LocalMcpServer::start(const QString& endpoint)
{
    if (_impl->thread.isRunning())
        return;
    _impl->queues = std::make_shared<Queues>();
    _impl->worker = new IoWorker(_impl->queues, this);
    _impl->worker->moveToThread(&_impl->thread);
    _impl->thread.setObjectName(QString::fromLatin1(IoThreadName));
    connect(&_impl->thread, &QThread::finished, _impl->worker, &QObject::deleteLater);
    connect(_impl->worker, &IoWorker::ready, this, &LocalMcpServer::listening);
    auto* worker = _impl->worker;
    _impl->thread.start();
    QMetaObject::invokeMethod(worker, [worker, endpoint] { worker->start(endpoint); }, Qt::QueuedConnection);
}

void LocalMcpServer::stop()
{
    {
        std::lock_guard<std::mutex> lock(_impl->queues->mutex);
        _impl->queues->stopping = true;
    }
    _impl->thread.quit();
    _impl->thread.wait();
    _impl->worker = nullptr;
}

bool LocalMcpServer::send(const QString& id, const QJsonObject& message)
{
    const auto bytes = frame(message);
    if (bytes.isEmpty())
        return false;
    bool schedule = false;
    {
        std::lock_guard<std::mutex> lock(_impl->queues->mutex);
        auto it = _impl->queues->pendingBytes.find(id);
        if (_impl->queues->stopping || it == _impl->queues->pendingBytes.end()
            || *it + bytes.size() > MaxOutputQueueBytes)
            return false;
        *it += bytes.size();
        const bool authenticated = message.value("op") == "hello" && message.value("ok").toBool();
        if (authenticated) _impl->queues->authenticatedIds.insert(id);
        _impl->queues->outgoing.push_back({id, bytes, false, authenticated});
        if (!_impl->queues->ioScheduled) { _impl->queues->ioScheduled = true; schedule = true; }
    }
    if (schedule && _impl->worker)
        QMetaObject::invokeMethod(_impl->worker, [worker = _impl->worker] { worker->flush(); }, Qt::QueuedConnection);
    return true;
}

void LocalMcpServer::closeConnection(const QString& id)
{
    bool schedule = false;
    {
        std::lock_guard<std::mutex> lock(_impl->queues->mutex);
        if (!_impl->queues->pendingBytes.contains(id))
            return;
        _impl->queues->outgoing.push_back({id, {}, true, false});
        if (!_impl->queues->ioScheduled) { _impl->queues->ioScheduled = true; schedule = true; }
    }
    if (schedule && _impl->worker)
        QMetaObject::invokeMethod(_impl->worker, [worker = _impl->worker] { worker->flush(); }, Qt::QueuedConnection);
}

void LocalMcpServer::drain()
{
    std::deque<Incoming> incoming;
    {
        std::lock_guard<std::mutex> lock(_impl->queues->mutex);
        incoming.swap(_impl->queues->incoming);
        _impl->queues->incomingBytes = 0;
        _impl->queues->guiScheduled = false;
    }
    for (const auto& item : incoming) {
        if (item.disconnected) {
            {
                std::lock_guard<std::mutex> lock(_impl->queues->mutex);
                _impl->queues->authenticatedIds.remove(item.id);
            }
            emit disconnected(item.id);
        } else
            emit packet(item.id, item.message);
    }
}
}
#include "LocalMcpServer.moc"
