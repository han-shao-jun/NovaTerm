/** @file StdioChannel.cpp
 *  @brief 跨平台管道取消、背压与合并事件投递。
 */
#include "StdioChannel.h"
#include "mcp/McpProtocol.h"
#include "core/ThreadNaming.h"
#include <QElapsedTimer>
#include <QTimer>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#endif

using namespace NovaTerm::Mcp;
class StdioChannel::Impl
{
public:
    explicit Impl(StdioChannel* owner) : q(owner) {}
    StdioChannel* q;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<QByteArray> incoming, outgoing;
    qsizetype incomingBytes{0}, outgoingBytes{0};
    std::atomic<bool> stopping{false};
    std::atomic<bool> readerDone{false}, writerDone{false};
    bool scheduled{false}, eof{false};
    std::thread reader, writer;
    QTimer watch;
    QElapsedTimer clock;
    std::atomic<qint64> lastProgress{0};
#ifndef Q_OS_WIN
    int stopPipe[2]{-1, -1};
    bool waitFd(int descriptor, short events)
    {
        pollfd descriptors[]{{descriptor, events, 0}, {stopPipe[0], POLLIN, 0}};
        while (!stopping) {
            const int result = ::poll(descriptors, 2, -1);
            if (result < 0 && errno == EINTR) continue;
            return result > 0 && descriptors[1].revents == 0
                && (descriptors[0].revents & (events | POLLHUP | POLLERR));
        }
        return false;
    }
#endif
    void notify()
    {
        // 调用方持锁，保证只存在一次尚未处理的 GUI/协议线程唤醒。
        if (!scheduled) {
            scheduled = true;
            QMetaObject::invokeMethod(q, [this] { q->drain(); }, Qt::QueuedConnection);
        }
    }
    bool push(QByteArray line)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (incoming.size() >= 16 || incomingBytes + line.size() > MaxFrameBytes)
            return false;
        incomingBytes += line.size();
        incoming.push_back(std::move(line));
        notify();
        return true;
    }
    void input()
    {
        NovaTerm::setCurrentThreadName("nvterm-mcp-in");
        QByteArray current;
        while (!stopping) {
            char buffer[16384];
            qint64 count = 0;
#ifdef Q_OS_WIN
            DWORD actual = 0;
            if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &actual, nullptr)) break;
            count = actual;
#else
            if (!waitFd(STDIN_FILENO, POLLIN)) break;
            count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
#endif
            if (count <= 0) break;
            bool failed = false;
            for (qint64 i = 0; i < count; ++i) {
                if (buffer[i] == '\n') {
                    if (!push(std::move(current))) { failed = true; break; }
                    current.clear();
                } else {
                    current += buffer[i];
                    if (current.size() > MaxFrameBytes) { failed = true; break; }
                }
            }
            if (failed) break;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            eof = true;
            notify();
        }
        readerDone = true;
    }
    void output()
    {
        NovaTerm::setCurrentThreadName("nvterm-mcp-out");
        while (!stopping) {
            QByteArray message;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return stopping || !outgoing.empty(); });
                if (stopping) break;
                message = std::move(outgoing.front());
                outgoing.pop_front();
            }
            qsizetype offset = 0;
            while (offset < message.size() && !stopping) {
                qint64 written = 0;
#ifdef Q_OS_WIN
                DWORD actual = 0;
                if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message.constData() + offset,
                    DWORD(std::min(qsizetype(16384), message.size() - offset)), &actual, nullptr)) break;
                written = actual;
#else
                if (!waitFd(STDOUT_FILENO, POLLOUT)) break;
                written = ::write(STDOUT_FILENO, message.constData() + offset,
                    size_t(std::min(qsizetype(16384), message.size() - offset)));
                if (written < 0 && (errno == EINTR || errno == EAGAIN)) continue;
#endif
                if (written <= 0) break;
                offset += qsizetype(written);
                lastProgress = clock.elapsed();
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                outgoingBytes -= message.size();
                if (offset < message.size()) { eof = true; notify(); break; }
            }
        }
        writerDone = true;
    }
};

StdioChannel::StdioChannel(QObject* parent) : QObject(parent), _impl(std::make_unique<Impl>(this))
{
    _impl->watch.setInterval(1000);
    connect(&_impl->watch, &QTimer::timeout, this, [this] {
        bool stalled = false;
        {
            std::lock_guard<std::mutex> lock(_impl->mutex);
            if (_impl->outgoingBytes == 0) { _impl->watch.stop(); return; }
            stalled = _impl->clock.elapsed() - _impl->lastProgress.load() > 10000;
        }
        if (stalled) emit ended();
    });
}
StdioChannel::~StdioChannel() { stop(); }
void StdioChannel::start()
{
    _impl->clock.start();
#ifndef Q_OS_WIN
    ::signal(SIGPIPE, SIG_IGN);
    if (::pipe(_impl->stopPipe) != 0) { emit ended(); return; }
    for (int fd : {STDIN_FILENO, STDOUT_FILENO})
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
#endif
    _impl->reader = std::thread([this] { _impl->input(); });
    _impl->writer = std::thread([this] { _impl->output(); });
}
void StdioChannel::stop()
{
    if (_impl->stopping.exchange(true)) return;
    _impl->watch.stop();
    _impl->wake.notify_all();
#ifdef Q_OS_WIN
    // 反复取消直到线程观察到停止位，覆盖“刚检查停止位、尚未进入 ReadFile”的竞争。
    while ((_impl->reader.joinable() && !_impl->readerDone)
        || (_impl->writer.joinable() && !_impl->writerDone)) {
        if (_impl->reader.joinable()) {
            CancelSynchronousIo(_impl->reader.native_handle());
            WaitForSingleObject(_impl->reader.native_handle(), 10);
        }
        if (_impl->writer.joinable()) {
            CancelSynchronousIo(_impl->writer.native_handle());
            WaitForSingleObject(_impl->writer.native_handle(), 10);
        }
    }
#else
    if (_impl->stopPipe[1] >= 0) { const char byte = 1; (void)::write(_impl->stopPipe[1], &byte, 1); }
#endif
    if (_impl->reader.joinable()) _impl->reader.join();
    if (_impl->writer.joinable()) _impl->writer.join();
#ifndef Q_OS_WIN
    for (int fd : _impl->stopPipe) if (fd >= 0) ::close(fd);
#endif
}
bool StdioChannel::write(const QByteArray& message)
{
    if (message.size() > MaxFrameBytes) return false;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        if (_impl->stopping || _impl->outgoingBytes + message.size() + 1 > MaxOutputQueueBytes)
            return false;
        if (_impl->outgoingBytes == 0) _impl->lastProgress = _impl->clock.elapsed();
        _impl->outgoingBytes += message.size() + 1;
        _impl->outgoing.push_back(message + '\n');
    }
    _impl->wake.notify_one();
    if (!_impl->watch.isActive()) _impl->watch.start();
    return true;
}
void StdioChannel::drain()
{
    std::deque<QByteArray> incoming;
    bool eof = false;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        incoming.swap(_impl->incoming);
        _impl->incomingBytes = 0;
        _impl->scheduled = false;
        eof = _impl->eof;
    }
    if (eof) { emit ended(); return; }
    for (const auto& message : incoming) emit line(message);
}
