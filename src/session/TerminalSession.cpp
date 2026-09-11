/**
 * @file   TerminalSession.cpp
 * @brief  终端会话实现：状态机、传输信号路由与输入泵管理。
 *
 * attach() 建立 Transport 信号 → 会话状态迁移的路由；disconnected 处理
 * 需用 generation 区分迟到的旧世代信号。transition() 经 isLegalTransition()
 * 校验，非法迁移告警并拒绝。
 */
#include "TerminalSession.h"

#include "SessionInputPump.h"
#include "core/terminal/TerminalCore.h"

#include <QDebug>

namespace {

/**
 * @brief 把传输层错误分类映射为会话层分类。
 *
 * 两个枚举语义基本一一对应；Resolve 归入 Connection（会话层不区分解析与
 * 建连），Overload 归入 InputOverload，Unknown 保守落到 Io。
 */
SessionErrorCategory toSessionCategory(TransportErrorCategory category)
{
    switch (category) {
    case TransportErrorCategory::Configuration:
        return SessionErrorCategory::Configuration;
    case TransportErrorCategory::Resolve:
    case TransportErrorCategory::Connection:
        return SessionErrorCategory::Connection;
    case TransportErrorCategory::Authentication:
        return SessionErrorCategory::Authentication;
    case TransportErrorCategory::HostKey:
        return SessionErrorCategory::HostKey;
    case TransportErrorCategory::Permission:
        return SessionErrorCategory::Permission;
    case TransportErrorCategory::Protocol:
        return SessionErrorCategory::Protocol;
    case TransportErrorCategory::Overload:
        return SessionErrorCategory::InputOverload;
    case TransportErrorCategory::Io:
    case TransportErrorCategory::Unknown:
        break;
    }
    return SessionErrorCategory::Io;
}

bool isReconnectInput(const QByteArray& data)
{
    // Enter 在普通模式下通常编码为 CR；兼容 LF/CRLF，避免不同键盘映射
    // 或粘贴单个换行时无法触发重连。
    return data == QByteArrayLiteral("\r") || data == QByteArrayLiteral("\n")
        || data == QByteArrayLiteral("\r\n");
}

bool isLegalTransition(SessionState from, SessionState to)
{
    switch (from) {
    case SessionState::Created:
        return to == SessionState::Connecting || to == SessionState::Closing;
    case SessionState::Connecting:
        return to == SessionState::Running || to == SessionState::Failed
            || to == SessionState::Closing;
    case SessionState::Running:
        return to == SessionState::Reconnecting || to == SessionState::Failed
            || to == SessionState::Closing;
    case SessionState::Reconnecting:
        return to == SessionState::Running || to == SessionState::Failed
            || to == SessionState::Closing;
    case SessionState::Failed:
        return to == SessionState::Reconnecting || to == SessionState::Closing;
    case SessionState::Closing:
        return to == SessionState::Closed;
    case SessionState::Closed:
        return false;
    }
    return false;
}

} // namespace

TerminalSession::TerminalSession(TerminalCore* core, QObject* parent)
    : QObject(parent)
    , _core(core)
{
    if (_core) {
        connect(_core, &TerminalCore::titleChanged,
                this, &TerminalSession::titleChanged);
        connect(_core, &TerminalCore::damage,
                this, [this] { emit activityChanged(); });
    }
}

TerminalSession::TerminalSession(RuntimeConfig config, QObject* parent)
    : QObject(parent)
    , _config(std::move(config))
    , _ownedCore(std::make_unique<TerminalCore>(80, 24))
    , _core(_ownedCore.get())
{
    connect(_core, &TerminalCore::titleChanged,
            this, &TerminalSession::titleChanged);
    connect(_core, &TerminalCore::damage,
            this, [this] { emit activityChanged(); });
}

TerminalSession::~TerminalSession()
{
    if (_state != SessionState::Closed)
        close(CloseMode::Abort);
    clearAttachment(false);
}

void TerminalSession::attach(ITransport* transport, Ownership ownership)
{
    attach(transport, ownership, _config.transportKind);
}

void TerminalSession::attach(ITransport* transport, Ownership ownership,
                             TransportKind transportKind)
{
    if (_transport == transport)
        return;
    clearAttachment(true);
    if (!transport || !_core)
        return;

    _transport = transport;
    _ownership = ownership;
    _config.transportKind = transportKind;
    if (ownership == Ownership::Adopt)
        transport->setParent(this);

    startPump();

    _coreOutputConnection = connect(
        _core, &TerminalCore::outputData, this,
        [this, transport](const QByteArray& data) {
            if (!_acceptsUserInput || _transport != transport)
                return;

            // 断连期间普通按键不应泄漏到旧链路；仅拦截 Enter，并按当前
            // 会话类型执行本地 Shell 重启或远端传输重连。
            if (_state == SessionState::Failed) {
                if (isReconnectInput(data) && canReconnect())
                    static_cast<void>(reconnect());
                return;
            }
            if (!transport->isConnected())
                return;
            _statistics.bytesSent += static_cast<quint64>(data.size());
            transport->write(data);
        });

    connectTransportSignals(transport, _statistics.generation);
}

void TerminalSession::rewireTransportSignals()
{
    if (!_transport)
        return;
    for (const auto& connection : std::as_const(_transportConnections))
        QObject::disconnect(connection);
    _transportConnections.clear();
    _transportConnections.reserve(6);
    connectTransportSignals(_transport.data(), _statistics.generation);
}

void TerminalSession::connectTransportSignals(ITransport* transport,
                                              quint64 generation)
{
    // 每个处理器同时校验 transport 指针与 generation：前者拦截 attach 换绑，
    // 后者拦截同一 transport 上一世代排队投递的迟到信号——SSH/Serial/Telnet
    // 重连复用同一 transport 对象，仅靠指针比较无法区分世代。
    const auto stale = [this, transport, generation]() {
        return _transport != transport || generation != _statistics.generation;
    };

    _transportConnections.append(connect(
        transport, &ITransport::readyRead, this,
        [this, stale](const QByteArray& bytes) {
            if (stale())
                return;
            _statistics.bytesReceived += static_cast<quint64>(bytes.size());
        }));
    _transportConnections.append(connect(
        transport, &ITransport::connected, this, [this, transport, stale] {
            if (stale())
                return;
            _statistics.connectedAt = QDateTime::currentDateTimeUtc();
            transition(SessionState::Running);
            emit connected(transport);
        }));
    // 结构化错误只做分类补充，不自行上报，避免与随后的 errorOccurred 重复
    // 产生两条 sessionError。约定见 ITransport.h。
    _transportConnections.append(connect(
        transport, &ITransport::transportError, this,
        [this, stale](const TransportError& error) {
            if (stale())
                return;
            _pendingTransportError = error;
        }));
    _transportConnections.append(connect(
        transport, &ITransport::errorOccurred, this,
        [this, transport, stale](const QString& message) {
            if (stale())
                return;
            // 有匹配的结构化错误时用其分类与可重试标志，否则回落到 Io。
            SessionErrorCategory category = SessionErrorCategory::Io;
            bool retryable = true;
            int code = 0;
            if (_pendingTransportError
                && _pendingTransportError->message == message) {
                category = toSessionCategory(_pendingTransportError->category);
                retryable = _pendingTransportError->retryable;
                code = _pendingTransportError->code;
            }
            _pendingTransportError.reset();
            reportError(category, message, retryable, code);
            // 建连阶段没有已建立链路可继续使用。部分后端只上报 errorOccurred
            // 而不会再发 disconnected，因此必须在这里结束 Connecting 状态。
            if ((_state == SessionState::Connecting
                 || _state == SessionState::Reconnecting)
                && !transport->isConnected()) {
                stopPump();
                transition(SessionState::Failed);
            }
            emit errorOccurred(transport, message);
        }));
    _transportConnections.append(connect(
        transport, &ITransport::exited, this,
        [this, transport, stale](quint32 exitCode, TransportExitReason reason) {
            if (!stale())
                emit exited(transport, exitCode, reason);
        }));
    _transportConnections.append(connect(
        transport, &ITransport::disconnected, this, [this, transport, stale] {
            if (stale())
                return;
            // 两道守卫覆盖不同的迟到场景，缺一不可：
            //  · stale() 拦截"发出时属于旧世代、投递时接线已重建"的信号；
            //  · 下面这条拦截跨线程投递——SshTransport/LocalShellTransport 用
            //    invokeMethod(QueuedConnection) 把 emit 本身推迟到 GUI 线程，
            //    emit 发生在重接线之后，generation 已是新值，仅靠 stale()
            //    无法识别；此时"transport 已连上"即证明这是上一世代的残留。
            // 彻底的解法需要把 generation 写进 ITransport 的信号契约，属于
            // 步骤 5 的接口变更，不在本次范围内。
            if (_state != SessionState::Closing && transport->isConnected())
                return;
            stopPump();
            if (_state == SessionState::Closing) {
                QObject::disconnect(_coreOutputConnection);
                _coreOutputConnection = {};
                _transport = nullptr;
                transition(SessionState::Closed);
                if (_ownership == Ownership::Adopt)
                    transport->deleteLater();
            } else if (_state != SessionState::Closed) {
                transition(SessionState::Failed);
            }
            emit disconnected(transport);
        }));
    // 对象销毁与世代无关（同一 transport 只会销毁一次），只校验指针。
    _transportConnections.append(connect(
        transport, &QObject::destroyed, this, [this, transport] {
            if (_transport != transport)
                return;
            stopPump();
            _transport = nullptr;
            if (_state == SessionState::Closing)
                transition(SessionState::Closed);
            else if (_state != SessionState::Closed)
                transition(SessionState::Failed);
            emit disconnected(nullptr);
        }));
}

bool TerminalSession::resetForReuse()
{
    if (_state != SessionState::Closed || _transport)
        return false;
    _sessionId = QUuid::createUuid();
    _contextProvider.reset();
    _state = SessionState::Created;
    _statistics = {};
    _acceptsUserInput = true;
    emit stateChanged(_state);
    return true;
}

void TerminalSession::detach()
{
    close(CloseMode::Graceful);
}

bool TerminalSession::start()
{
    if (!_transport || (_state != SessionState::Created
                        && _state != SessionState::Failed)) {
        return false;
    }
    _acceptsUserInput = true;
    startPump();
    if (_state == SessionState::Created)
        transition(SessionState::Connecting);
    else
        transition(SessionState::Reconnecting);
    ++_statistics.generation;
    rewireTransportSignals();
    if (_transport->connectAsync())
        return true;
    stopPump();
    reportError(SessionErrorCategory::Connection, _transport->errorString(), true);
    transition(SessionState::Failed);
    return false;
}

void TerminalSession::close(CloseMode mode)
{
    Q_UNUSED(mode);
    if (_state == SessionState::Closed || _state == SessionState::Closing)
        return;
    _acceptsUserInput = false;
    transition(SessionState::Closing);
    stopPump();
    if (!_transport) {
        transition(SessionState::Closed);
        return;
    }
    ITransport* current = _transport.data();
    current->setReadPaused(true);
    current->disconnect();
    if (!current->hasPendingDisconnect() && _transport == current) {
        clearAttachment(false);
        transition(SessionState::Closed);
        emit disconnected(current);
        if (_ownership == Ownership::Adopt)
            current->deleteLater();
    }
}

bool TerminalSession::reconnect()
{
    if (!canReconnect())
        return false;

    if (_transport->isConnected())
        _transport->disconnect();
    if (!_transport)
        return false;

    // 本地 Shell 必须走 start() 重新创建 PTY/子进程；SSH、串口和
    // Telnet 则复用当前 transport 重新打开远端链路。Custom 仅在声明
    // Reconnect 能力时走通用入口，避免对未知后端作不安全假设。
    switch (_config.transportKind) {
    case TransportKind::LocalShell:
        // 本地进程的关闭是异步的；只有 disconnected 已把会话推进到
        // Failed 后才能安全复用原配置启动下一代进程。
        if (_state != SessionState::Failed)
            return false;
        ++_statistics.reconnectCount;
        return start();
    case TransportKind::Ssh:
    case TransportKind::Serial:
    case TransportKind::Telnet:
    case TransportKind::Custom:
        return beginReconnect();
    }
    return false;
}

bool TerminalSession::canReconnect() const noexcept
{
    if (!_transport || (_state != SessionState::Running
                        && _state != SessionState::Failed)) {
        return false;
    }
    if (!_transport->capabilities().testFlag(TransportCapability::Reconnect))
        return false;

    switch (_config.transportKind) {
    case TransportKind::LocalShell:
    case TransportKind::Ssh:
    case TransportKind::Serial:
    case TransportKind::Telnet:
    case TransportKind::Custom:
        return true;
    }
    return false;
}

bool TerminalSession::beginReconnect()
{
    if (!_transport || !transition(SessionState::Reconnecting))
        return false;

    ++_statistics.reconnectCount;
    ++_statistics.generation;
    rewireTransportSignals();
    startPump();
    if (_transport->connectAsync())
        return true;

    const QString message = _transport->errorString();
    stopPump();
    reportError(SessionErrorCategory::Connection, message, true);
    transition(SessionState::Failed);
    return false;
}

void TerminalSession::write(const QByteArray& data)
{
    if (_acceptsUserInput && _transport && _transport->isConnected()) {
        _statistics.bytesSent += static_cast<quint64>(data.size());
        _transport->write(data);
    }
}

void TerminalSession::resize(int columns, int rows)
{
    if (_transport)
        _transport->resizeTerminal(columns, rows);
}

bool TerminalSession::transition(SessionState next)
{
    if (_state == next)
        return true;
    if (!isLegalTransition(_state, next)) {
        qWarning() << "Illegal session state transition" << static_cast<int>(_state)
                   << "->" << static_cast<int>(next) << _sessionId;
        return false;
    }
    _state = next;
    emit stateChanged(_state);
    return true;
}

void TerminalSession::stopPump()
{
    if (!_inputPump)
        return;
    delete _inputPump;
    _inputPump = nullptr;
}

void TerminalSession::startPump()
{
    if (_inputPump || !_transport || !_core)
        return;
    _inputPump = new SessionInputPump(_transport, _core, this);
    connect(_inputPump, &SessionInputPump::overload, this,
            [this](const QString& reason) {
        reportError(SessionErrorCategory::InputOverload, reason);
        if (_state == SessionState::Running)
            transition(SessionState::Failed);
    });
    _inputPump->start();
}

void TerminalSession::clearAttachment(bool requestDisconnect)
{
    ITransport* current = _transport.data();
    stopPump();
    QObject::disconnect(_coreOutputConnection);
    _coreOutputConnection = {};
    for (const auto& connection : std::as_const(_transportConnections))
        QObject::disconnect(connection);
    _transportConnections.clear();
    _pendingTransportError.reset();
    _transport = nullptr;
    if (!current)
        return;
    if (requestDisconnect)
        current->disconnect();
    if (_ownership == Ownership::Adopt)
        current->deleteLater();
}

void TerminalSession::reportError(SessionErrorCategory category,
                                  const QString& message, bool retryable,
                                  int code)
{
    emit sessionError(SessionError{category, code, message, retryable});
}
