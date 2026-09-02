/**
 * @file   TelnetTransport.cpp
 * @brief  Telnet 字节传输实现。
 *
 * QTcpSocket 负责链路，libtelnet 负责协议：入站字节喂给 telnet_recv()，
 * 由回调吐出净数据（TELNET_EV_DATA）与待发字节（TELNET_EV_SEND）；出站
 * 数据经 telnet_send_text() 完成 IAC 转义与 NVT 换行处理。
 *
 * libtelnet 的 telopt 表只决定"是否接受对端发起的协商"，不会主动发起，
 * 因此连接建立后需显式调用 telnet_negotiate() 申报客户端意向。
 */
#include "TelnetTransport.h"

#include <libtelnet.h>

#include <QDebug>

#include <array>
#include <utility>

namespace {

/// NAWS 子协商每个维度为 16 位无符号，超出范围按上限截断。
constexpr int MaxNawsDimension = 0xFFFF;

} // namespace

// ═══════════════════════════════════════════════════════════════════
//  Impl —— 协议状态机（libtelnet 类型仅在本文件内可见）
// ═══════════════════════════════════════════════════════════════════

class TelnetTransport::Impl
{
public:
    explicit Impl(TelnetTransport* owner) : _owner(owner) {}
    ~Impl() { destroy(); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    /// 创建状态机并按配置申报客户端协商意向。
    bool create(const TelnetConfig& config);
    /// 释放状态机（可重复调用）。
    void destroy();
    [[nodiscard]] bool ready() const noexcept { return _telnet != nullptr; }

    void feed(const char* data, std::size_t size);
    void sendText(const char* data, std::size_t size);
    void sendKeepAlive();
    void sendNaws(int columns, int rows);

    /// 取出并清空本轮 telnet_recv() 累积的净数据。
    [[nodiscard]] QByteArray takeInbound()
    {
        return std::exchange(_inbound, QByteArray{});
    }
    [[nodiscard]] bool hasInbound() const noexcept { return !_inbound.isEmpty(); }
    [[nodiscard]] bool nawsNegotiated() const noexcept { return _nawsNegotiated; }
    [[nodiscard]] bool binaryNegotiated() const noexcept { return _binaryNegotiated; }

private:
    static void trampoline(telnet_t* telnet, telnet_event_t* event, void* userData);
    void onEvent(const telnet_event_t& event);

    TelnetTransport* _owner;
    telnet_t* _telnet{nullptr};

    // libtelnet 只保存本表的指针（libtelnet.c 中的 telnet->telopts），此后
    // 每次协商都会回读，因此该表必须与 telnet_t 同寿命，不能是临时变量。
    // 容量 6 = SGA + ECHO + TTYPE + NAWS + BINARY + 结束标记。
    std::array<telnet_telopt_t, 6> _telopts{};

    QByteArray _inbound;            ///< 单轮解析累积的净数据
    bool _nawsNegotiated{false};    ///< 对端是否回了 DO NAWS
    bool _binaryNegotiated{false};  ///< 发送方向是否已进入 BINARY
};

bool TelnetTransport::Impl::create(const TelnetConfig& config)
{
    destroy();

    // 客户端协商能力表：us = 我方是否愿意 WILL，him = 是否接受对端 DO。
    // 该表只用于应答对端发起的协商（libtelnet 的 _check_telopt）。
    // 不申报 NEW-ENVIRON：UI 未提供环境变量输入，申报后无值可送。
    std::size_t count = 0;
    _telopts[count++] = {TELNET_TELOPT_SGA, TELNET_WILL, TELNET_DO};
    // 回显固定交给远端：对端若请求我方回显（DO ECHO）一律拒绝。
    _telopts[count++] = {TELNET_TELOPT_ECHO, TELNET_WONT, TELNET_DO};
    _telopts[count++] = {TELNET_TELOPT_TTYPE, TELNET_WILL, TELNET_DONT};
    if (config.naws)
        _telopts[count++] = {TELNET_TELOPT_NAWS, TELNET_WILL, TELNET_DONT};
    if (config.binaryMode)
        _telopts[count++] = {TELNET_TELOPT_BINARY, TELNET_WILL, TELNET_DO};
    _telopts[count] = {-1, 0, 0};

    _inbound.clear();
    _nawsNegotiated = false;
    _binaryNegotiated = false;

    _telnet = telnet_init(_telopts.data(), &Impl::trampoline, 0, this);
    if (_telnet == nullptr)
        return false;

    // 主动申报意向。填表不会发起协商，必须显式 telnet_negotiate()。
    // 字符模式的关键是 SGA 双向 + 远端 ECHO；缺一就会退化为行模式。
    telnet_negotiate(_telnet, TELNET_DO, TELNET_TELOPT_SGA);
    telnet_negotiate(_telnet, TELNET_WILL, TELNET_TELOPT_SGA);
    telnet_negotiate(_telnet, TELNET_DO, TELNET_TELOPT_ECHO);
    telnet_negotiate(_telnet, TELNET_WILL, TELNET_TELOPT_TTYPE);
    if (config.naws)
        telnet_negotiate(_telnet, TELNET_WILL, TELNET_TELOPT_NAWS);
    if (config.binaryMode) {
        telnet_negotiate(_telnet, TELNET_WILL, TELNET_TELOPT_BINARY);
        telnet_negotiate(_telnet, TELNET_DO, TELNET_TELOPT_BINARY);
    }
    return true;
}

void TelnetTransport::Impl::destroy()
{
    if (_telnet != nullptr) {
        telnet_free(_telnet);
        _telnet = nullptr;
    }
    _inbound.clear();
    _nawsNegotiated = false;
    _binaryNegotiated = false;
}

void TelnetTransport::Impl::feed(const char* data, std::size_t size)
{
    if (_telnet != nullptr && size > 0)
        telnet_recv(_telnet, data, size);
}

void TelnetTransport::Impl::sendText(const char* data, std::size_t size)
{
    if (_telnet == nullptr || size == 0)
        return;
    // telnet_send_text 内部按已协商的 TELNET_FLAG_TRANSMIT_BINARY 决定行为：
    // 未协商 BINARY 时 CR→CR NUL、LF→CR LF（RFC 854 NVT），协商后原样透传；
    // 两种模式都会把 0xFF 加倍转义。
    telnet_send_text(_telnet, data, size);
}

void TelnetTransport::Impl::sendKeepAlive()
{
    if (_telnet != nullptr)
        telnet_iac(_telnet, TELNET_NOP);
}

void TelnetTransport::Impl::sendNaws(int columns, int rows)
{
    if (_telnet == nullptr || columns <= 0 || rows <= 0)
        return;

    const int cols = qMin(columns, MaxNawsDimension);
    const int lines = qMin(rows, MaxNawsDimension);
    // RFC 1073：SB NAWS <宽高各 2 字节大端> SE。payload 中的 0xFF 由
    // telnet_subnegotiation() 内部经 telnet_send() 加倍，宽度为 255 时安全。
    const unsigned char payload[4] = {
        static_cast<unsigned char>((cols >> 8) & 0xFF),
        static_cast<unsigned char>(cols & 0xFF),
        static_cast<unsigned char>((lines >> 8) & 0xFF),
        static_cast<unsigned char>(lines & 0xFF),
    };
    telnet_subnegotiation(_telnet, TELNET_TELOPT_NAWS,
                          reinterpret_cast<const char*>(payload), sizeof(payload));
}

void TelnetTransport::Impl::trampoline(telnet_t* telnet, telnet_event_t* event,
                                       void* userData)
{
    Q_UNUSED(telnet);
    auto* self = static_cast<Impl*>(userData);
    if (self != nullptr && event != nullptr)
        self->onEvent(*event);
}

void TelnetTransport::Impl::onEvent(const telnet_event_t& event)
{
    switch (event.type) {
    case TELNET_EV_DATA:
        // 净数据先累积，待 telnet_recv() 返回后统一投递：同一 chunk 内可能
        // 既有数据又有协商，在解析栈内 emit 会让调用方重入 libtelnet。
        if (event.data.size > 0)
            _inbound.append(event.data.buffer,
                            static_cast<qsizetype>(event.data.size));
        break;

    case TELNET_EV_SEND:
        // 库要求发出的字节（协商应答、转义后的用户数据）直写 socket。
        _owner->writeRaw(event.data.buffer,
                         static_cast<qsizetype>(event.data.size));
        break;

    case TELNET_EV_DO:
        if (event.neg.telopt == TELNET_TELOPT_NAWS) {
            _nawsNegotiated = true;
            // 对端刚接受 NAWS，立即补报一次当前尺寸，不必等下一次 resize。
            if (_owner->_columns > 0 && _owner->_rows > 0)
                sendNaws(_owner->_columns, _owner->_rows);
        } else if (event.neg.telopt == TELNET_TELOPT_BINARY) {
            _binaryNegotiated = true;
        }
        break;

    case TELNET_EV_DONT:
        if (event.neg.telopt == TELNET_TELOPT_NAWS)
            _nawsNegotiated = false;
        else if (event.neg.telopt == TELNET_TELOPT_BINARY)
            _binaryNegotiated = false;
        break;

    case TELNET_EV_TTYPE:
        // 对端询问终端类型（SEND）→ 回 IS <terminalType>。
        if (event.ttype.cmd == TELNET_TTYPE_SEND) {
            const QByteArray ttype =
                _owner->_config.terminalType.trimmed().toLatin1();
            telnet_ttype_is(_telnet, ttype.isEmpty() ? "xterm-256color"
                                                     : ttype.constData());
        }
        break;

    case TELNET_EV_WARNING:
        // 可恢复的协议异常（如超长子协商被丢弃）。不上抛 errorOccurred：
        // ITransport 没有独立告警通道，把良性告警升级为会话级错误会误伤。
        qWarning() << "Telnet protocol warning:"
                   << (event.error.msg != nullptr ? event.error.msg : "");
        break;

    case TELNET_EV_ERROR:
        _owner->reportError(
            TransportErrorCategory::Protocol,
            static_cast<int>(event.error.errcode),
            TelnetTransport::tr("Telnet protocol error: %1")
                .arg(QString::fromUtf8(event.error.msg != nullptr
                                           ? event.error.msg : "")),
            false);
        break;

    default:
        // IAC / SUBNEGOTIATION / COMPRESS / ZMP / ENVIRON / MSSP：终端客户端
        // 无需额外处理，libtelnet 已在协议层完成应答。
        break;
    }
}

// ═══════════════════════════════════════════════════════════════════
//  TelnetTransport
// ═══════════════════════════════════════════════════════════════════

TelnetTransport::TelnetTransport(TelnetConfig config, QObject* parent)
    : ITransport(parent)
    , _impl(std::make_unique<Impl>(this))
    , _config(std::move(config))
{
    // 有界读缓冲：暂停 drain 时 TCP 接收窗口随之收缩，由对端自然减速。
    _socket.setReadBufferSize(ReadBufferBytes);

    connect(&_socket, &QTcpSocket::connected,
            this, &TelnetTransport::handleSocketConnected);
    connect(&_socket, &QTcpSocket::disconnected,
            this, &TelnetTransport::handleSocketDisconnected);
    connect(&_socket, &QTcpSocket::readyRead,
            this, &TelnetTransport::readAvailable);
    connect(&_socket, &QTcpSocket::errorOccurred,
            this, &TelnetTransport::handleSocketError);
    connect(&_socket, &QTcpSocket::bytesWritten,
            this, &TelnetTransport::bytesWritten);

    _keepAliveTimer.setSingleShot(false);
    connect(&_keepAliveTimer, &QTimer::timeout,
            this, &TelnetTransport::sendKeepAlive);
}

TelnetTransport::~TelnetTransport()
{
    // 先停表并释放状态机，避免析构过程中仍有回调写入 socket。
    _keepAliveTimer.stop();
    _impl->destroy();
    if (_socket.state() != QAbstractSocket::UnconnectedState)
        _socket.abort();
}

bool TelnetTransport::connectToHost()
{
    if (_socket.state() != QAbstractSocket::UnconnectedState)
        disconnect();

    if (!_config.isValid()) {
        reportError(TransportErrorCategory::Configuration, 0,
                    tr("Invalid Telnet configuration."), false);
        return false;
    }

    _errorString.clear();
    _readPaused = false;
    _disconnectEmitted = false;
    // QTcpSocket::connectToHost 本身异步，结果经 connected()/errorOccurred()
    // 在事件循环中回调，无需像 QSerialPort::open 那样手工延迟。
    _socket.connectToHost(_config.host.trimmed(), _config.port);
    return true;
}

void TelnetTransport::disconnect()
{
    const bool wasActive = _socket.state() != QAbstractSocket::UnconnectedState;
    teardown();

    if (wasActive && !_disconnectEmitted) {
        _disconnectEmitted = true;
        emit disconnected();
    }
}

void TelnetTransport::write(const QByteArray& data)
{
    if (data.isEmpty())
        return;
    if (!isConnected() || !_impl->ready()) {
        reportError(TransportErrorCategory::Io, 0,
                    tr("Telnet session is not connected."), false);
        return;
    }
    if (_socket.bytesToWrite() + data.size() > MaxPendingWriteBytes) {
        reportError(TransportErrorCategory::Overload, 0,
                    tr("Telnet write queue exceeded its 1 MiB limit."), false);
        return;
    }

    _impl->sendText(data.constData(), static_cast<std::size_t>(data.size()));
}

void TelnetTransport::resizeTerminal(int cols, int rows)
{
    if (cols <= 0 || rows <= 0)
        return;

    // 尺寸始终缓存：连接前调用作为初始尺寸，NAWS 协商成功后立即补报。
    _columns = cols;
    _rows = rows;

    if (_config.naws && _impl->ready() && _impl->nawsNegotiated())
        _impl->sendNaws(cols, rows);
}

bool TelnetTransport::isConnected() const
{
    return _socket.state() == QAbstractSocket::ConnectedState;
}

QString TelnetTransport::errorString() const
{
    return _errorString;
}

bool TelnetTransport::setReadPaused(bool paused)
{
    _readPaused = paused;
    if (!paused)
        readAvailable();
    return true;
}

bool TelnetTransport::nawsNegotiated() const noexcept
{
    return _impl->nawsNegotiated();
}

bool TelnetTransport::binaryNegotiated() const noexcept
{
    return _impl->binaryNegotiated();
}

void TelnetTransport::handleSocketConnected()
{
    if (!_impl->create(_config)) {
        reportError(TransportErrorCategory::Unknown, 0,
                    tr("Cannot initialise the Telnet protocol handler."), false);
        teardown();
        return;
    }

    if (_config.keepAliveSeconds > 0)
        _keepAliveTimer.start(_config.keepAliveSeconds * 1000);

    emit connected();
}

void TelnetTransport::handleSocketDisconnected()
{
    teardown();
    if (!_disconnectEmitted) {
        _disconnectEmitted = true;
        emit disconnected();
    }
}

void TelnetTransport::handleSocketError(QAbstractSocket::SocketError error)
{
    // 对端正常关闭会同时给出 RemoteHostClosedError 与 disconnected()，
    // 交给 handleSocketDisconnected() 处理，不作为错误上报。
    if (error == QAbstractSocket::RemoteHostClosedError)
        return;

    TransportErrorCategory category = TransportErrorCategory::Unknown;
    bool retryable = false;
    switch (error) {
    case QAbstractSocket::HostNotFoundError:
        category = TransportErrorCategory::Resolve;
        break;
    case QAbstractSocket::ConnectionRefusedError:
    case QAbstractSocket::SocketTimeoutError:
    case QAbstractSocket::NetworkError:
        category = TransportErrorCategory::Connection;
        retryable = true;
        break;
    case QAbstractSocket::SocketAccessError:
        category = TransportErrorCategory::Permission;
        break;
    default:
        category = TransportErrorCategory::Io;
        break;
    }

    reportError(category, static_cast<int>(error),
                tr("Telnet %1:%2 — %3")
                    .arg(_config.host)
                    .arg(_config.port)
                    .arg(_socket.errorString()),
                retryable);

    // 连接类错误不会再产生 disconnected()，需在此收尾以便上层进入可重连状态。
    if (_socket.state() != QAbstractSocket::ConnectedState)
        disconnect();
}

void TelnetTransport::readAvailable()
{
    if (_readPaused || !_impl->ready())
        return;

    while (!_readPaused && _impl->ready() && _socket.bytesAvailable() > 0) {
        const QByteArray chunk = _socket.read(ReadChunkBytes);
        if (chunk.isEmpty())
            break;

        _impl->feed(chunk.constData(), static_cast<std::size_t>(chunk.size()));

        // 解析返回后才投递净数据：此时不在 libtelnet 栈内，调用方即使在
        // 槽函数里回调 write()/disconnect() 也不会破坏解析状态。
        if (_impl->hasInbound())
            emit readyRead(_impl->takeInbound());
    }
}

void TelnetTransport::sendKeepAlive()
{
    if (isConnected() && _impl->ready())
        _impl->sendKeepAlive();
}

void TelnetTransport::writeRaw(const char* data, qsizetype size)
{
    if (data == nullptr || size <= 0)
        return;
    if (_socket.state() != QAbstractSocket::ConnectedState)
        return;

    if (_socket.write(data, size) < 0) {
        reportError(TransportErrorCategory::Io, 0,
                    tr("Telnet write failed: %1").arg(_socket.errorString()),
                    true);
    }
}

void TelnetTransport::teardown()
{
    _keepAliveTimer.stop();
    _impl->destroy();
    _readPaused = false;
    if (_socket.state() != QAbstractSocket::UnconnectedState) {
        // ITransport::disconnect() 约定"已缓冲的写入尽力刷新但无保证"：
        // 先尝试冲刷，再 abort() 立即释放，不等待四次挥手。
        _socket.flush();
        _socket.abort();
    }
}

void TelnetTransport::reportError(TransportErrorCategory category, int code,
                                  const QString& message, bool retryable)
{
    _errorString = message;
    // 先发结构化错误：TerminalSession 据此为紧随其后的 errorOccurred 补充
    // 分类（约定见 ITransport.h），顺序颠倒会导致分类回落到 Io。
    emit transportError(TransportError{category, code, message, retryable});
    emit errorOccurred(_errorString);
}
