/**
 * @file SshWorkerWakeup.h
 * @brief 有界、合并的 worker 唤醒 socket；不调用 libssh。
 */
#pragma once

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#endif
#include <mutex>

class SshWorkerWakeup final
{
public:
#ifdef _WIN32
    using Socket = SOCKET;
    static constexpr Socket Invalid = INVALID_SOCKET;
#else
    using Socket = int;
    static constexpr Socket Invalid = -1;
#endif
    SshWorkerWakeup()
    {
#ifdef _WIN32
        WSADATA data{};
        _initialized = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        if (!_initialized)
            return;
        // Windows 的 libssh poll 接受 socket，使用本地 TCP 对保证唤醒可靠交付。
        const Socket listener = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        if (!bindLocal(listener, address) || listen(listener, 1) != 0) {
            closeSocket(listener);
            return;
        }
        _writer = socket(AF_INET, SOCK_STREAM, 0);
        if (_writer == Invalid || ::connect(_writer,
                reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            closeSocket(listener);
            return;
        }
        _reader = accept(listener, nullptr, nullptr);
        closeSocket(listener);
        if (_reader == Invalid)
            return;
#else
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
            return;
        _reader = pair[0];
        _writer = pair[1];
#endif
        _valid = nonblocking(_reader) && nonblocking(_writer);
    }
    ~SshWorkerWakeup()
    {
        closeSocket(_reader);
        closeSocket(_writer);
#ifdef _WIN32
        if (_initialized)
            WSACleanup();
#endif
    }
    SshWorkerWakeup(const SshWorkerWakeup&) = delete;
    SshWorkerWakeup& operator=(const SshWorkerWakeup&) = delete;
    SshWorkerWakeup(SshWorkerWakeup&&) = delete;
    SshWorkerWakeup& operator=(SshWorkerWakeup&&) = delete;

    [[nodiscard]] bool valid() const noexcept { return _valid; }
    [[nodiscard]] Socket descriptor() const noexcept { return _reader; }
    void notify()
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (_valid && !_pending)
            _pending = ::send(_writer, "w", 1, 0) == 1;
    }
    void consume()
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        char byte;
        if (_pending && ::recv(_reader, &byte, 1, 0) == 1)
            _pending = false;
    }

private:
    static bool bindLocal(Socket fd, sockaddr_in& address)
    {
        if (fd == Invalid)
            return false;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            return false;
#ifdef _WIN32
        int length = sizeof(address);
#else
        socklen_t length = sizeof(address);
#endif
        return getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    }
    static bool nonblocking(Socket fd)
    {
#ifdef _WIN32
        u_long enabled = 1;
        return ioctlsocket(fd, FIONBIO, &enabled) == 0;
#else
        const int flags = fcntl(fd, F_GETFL, 0);
        return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0
            && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
#endif
    }
    static void closeSocket(Socket fd)
    {
        if (fd == Invalid)
            return;
#ifdef _WIN32
        closesocket(fd);
#else
        ::close(fd);
#endif
    }
    Socket _reader{Invalid};
    Socket _writer{Invalid};
    bool _valid{false};
    bool _pending{false};
#ifdef _WIN32
    bool _initialized{false};
#endif
    std::mutex _mutex;
};
