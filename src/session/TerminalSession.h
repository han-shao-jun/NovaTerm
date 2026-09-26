/**
 * @file   TerminalSession.h
 * @brief  终端会话：聚合 TerminalCore 与 ITransport 的运行时单元。
 *
 * 拥有一个终端运行时（TerminalCore）与一条传输连接（ITransport），通过
 * SessionInputPump 在二者间转送字节并施加背压。本类刻意不依赖 TerminalView、
 * QWidget 或渲染资源，可独立创建/恢复/销毁。
 */
#pragma once

#include "SessionTypes.h"
#include "TerminalContextProvider.h"
#include "transport/ITransport.h"

#include <QObject>
#include <QChronoTimer>
#include <QPointer>
#include <QVector>
#include <memory>
#include <optional>

class SessionInputPump;
class SessionCommandFacade;
class SessionInputArbiter;
class InteractiveStreamFramer;
class SessionCommandCoordinator;
class TerminalCore;

/**
 * @brief 终端会话：一个终端运行时 + 一条传输连接。
 *
 * 管理 TerminalCore 与 ITransport 的生命周期、信号连接与状态机迁移。
 * 不持有任何 UI/渲染资源，可独立创建/恢复/销毁。当前架构下由拥有它的
 * TerminalView 管理其生命周期（1 View : 1 Session）。
 */
class TerminalSession final : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief 设置串口自动重连间隔，须在启动连接前调用。
     * @param seconds 非负整秒数，0 禁用自动重连。
     */
    void setSerialReconnectInterval(int seconds)
    {
        _config.transport.insert(QStringLiteral("reconnectSeconds"), seconds);
    }
    /**
     * @brief transport 所有权模式。
     */
    enum class Ownership { Borrowed, Adopt };

    /**
     * @brief 构造一个借用外部 TerminalCore 的会话。
     * @param core   外部拥有的终端核心（本会话不接管其所有权）。
     * @param parent 父对象。
     */
    explicit TerminalSession(TerminalCore* core, QObject* parent = nullptr);

    /**
     * @brief 构造一个内部拥有 TerminalCore 的会话。
     * @param config 运行时配置。
     * @param parent 父对象。
     */
    explicit TerminalSession(RuntimeConfig config, QObject* parent = nullptr);
    ~TerminalSession() override;

    /** @brief 会话唯一 ID。 */
    [[nodiscard]] SessionId id() const noexcept { return _sessionId; }
    /** @brief 当前会话状态。 */
    [[nodiscard]] SessionState state() const noexcept { return _state; }
    /** @brief 运行时配置。 */
    [[nodiscard]] const RuntimeConfig& runtimeConfig() const noexcept { return _config; }
    /** @brief 会话统计。 */
    [[nodiscard]] const SessionStatistics& statistics() const noexcept { return _statistics; }
    /** @brief 终端核心指针。 */
    [[nodiscard]] TerminalCore* core() const noexcept { return _core; }
    /** @brief 按需提供有界 Agent 上下文；调用方只消费解析后的文本。 */
    [[nodiscard]] TerminalContextProvider::Context terminalContext(
        const TerminalContextProvider::Request& request)
    {
        if (!_contextProvider)
            _contextProvider = std::make_unique<TerminalContextProvider>(_core);
        return _contextProvider->context(request);
    }
    /** @brief 非阻塞取得共享基础摘要；只能在 Session 所在线程调用。 */
    [[nodiscard]] std::shared_ptr<const TerminalContextProvider::Snapshot> tryTerminalContext()
    {
        if (!_contextProvider)
            _contextProvider = std::make_unique<TerminalContextProvider>(_core);
        return _contextProvider->trySnapshot();
    }
    /** @brief 传输层指针（未附加时为 nullptr）。 */
    [[nodiscard]] ITransport* transport() const { return _transport.data(); }
    /** @brief Session 级受限命令门面；具体 Executor 由编排层安装。 */
    [[nodiscard]] SessionCommandFacade* commandFacade() const noexcept
    {
        return _commandFacade.get();
    }
    /** @brief Session 出站输入仲裁器；供受限交互命令门面使用。 */
    [[nodiscard]] SessionInputArbiter* inputArbiter() const noexcept
    {
        return _inputArbiter.get();
    }
    /** @brief 当前 Session 的交互命令协调器。 */
    [[nodiscard]] SessionCommandCoordinator* commandCoordinator() const noexcept
    {
        return _commandCoordinator.get();
    }

    /**
     * @brief 附加传输层。
     * @param transport 传输层。
     * @param ownership 所有权模式（Adopt 表示会话接管其 deleteLater）。
     * @note 会先清理已有附加，再建立信号连接并启动输入泵。
     */
    void attach(ITransport* transport, Ownership ownership = Ownership::Adopt);

    /**
     * @brief 附加传输层并显式记录后端类型。
     * @param transport     传输层。
     * @param ownership     所有权模式。
     * @param transportKind 当前传输后端类型。
     */
    void attach(ITransport* transport, Ownership ownership,
                TransportKind transportKind);

    /**
     * @brief 重置会话以复用（在到达 Closed 后开启新逻辑连接）。
     * @return true 表示重置成功；会话未到 Closed 或仍有 transport 附加时返回 false。
     */
    bool resetForReuse();

    /**
     * @brief 分离传输层（优雅关闭后解除附加）。
     */
    void detach();

    /**
     * @brief 启动连接（Created/Failed 状态可调用）。
     * @return true 表示已请求连接；状态不合法返回 false。
     */
    [[nodiscard]] bool start();

    /**
     * @brief 关闭会话。
     * @param mode 关闭模式（Graceful 等待清理，Abort 立即中止）。
     */
    void close(CloseMode mode = CloseMode::Graceful);

    /**
     * @brief 主动断开当前传输，但保留附加关系供后续重连。
     * @return 仅 Running、传输已连接且支持重连时返回 true。
     * @note 与 close() 不同，本操作不会进入 Closing/Closed，也不会销毁 Transport。
     */
    [[nodiscard]] bool disconnectForReconnect();

    /**
     * @brief 重连（Running/Failed 状态可调用）。
     * @return true 表示已请求重连；状态不合法返回 false。
     */
    [[nodiscard]] bool reconnect();

    /**
     * @brief 当前会话是否支持重新连接。
     * @return 传输存在、状态允许且后端声明了 Reconnect 能力时返回 true。
     */
    [[nodiscard]] bool canReconnect() const noexcept;

    /**
     * @brief 写入用户输入到传输层。
     * @param data 待发送字节。
     */
    void write(const QByteArray& data);

    /// @copydoc write
    void writeUserInput(const QByteArray& data) { write(data); }

    /**
     * @brief 调整远程终端尺寸。
     * @param columns 列数。
     * @param rows    行数。
     */
    void resize(int columns, int rows);

    /// @copydoc resize
    void resizeTerminal(int columns, int rows) { resize(columns, rows); }

signals:
    /** @brief 定时自动重连开始一次尝试，供视图更新等待提示。 */
    void automaticReconnectAttemptStarted();
    /** @brief 定时自动重连成功；首次连接和手动重连不发出此信号。 */
    void automaticReconnectSucceeded();
    /**
     * @brief 会话状态变更。
     * @param state 新状态。
     */
    void stateChanged(SessionState state);

    /**
     * @brief 会话发生结构化错误。
     * @param error 错误对象。
     */
    void sessionError(const SessionError& error);

    /**
     * @brief 会话标题变更（由终端核心的标题转义序列触发）。
     * @param title 新标题。
     */
    void titleChanged(const QString& title);

    /**
     * @brief 会话有活动（数据收发、屏幕刷新等）。
     */
    void activityChanged();

    /**
     * @brief 传输层已连接。
     * @param transport 触发的传输层。
     */
    void connected(ITransport* transport);

    /**
     * @brief 传输层已断开。
     * @param transport 触发的传输层（被销毁时为 nullptr）。
     */
    void disconnected(ITransport* transport);

    /**
     * @brief 传输层发生错误。
     * @param transport 触发的传输层。
     * @param error     错误描述。
     */
    void errorOccurred(ITransport* transport, const QString& error);

    /**
     * @brief 传输层子进程/会话退出。
     * @param transport 触发的传输层。
     * @param exitCode  退出码。
     * @param reason    退出原因。
     */
    void exited(ITransport* transport, quint32 exitCode,
                TransportExitReason reason);

private:
    QChronoTimer _reconnectTimer; ///< 单次自动重连定时器，支持完整整秒范围
    bool _manualDisconnect{false}; ///< 主动断开后暂停自动重连
    std::unique_ptr<TerminalContextProvider> _contextProvider;
    std::unique_ptr<SessionCommandFacade> _commandFacade;
    std::unique_ptr<SessionInputArbiter> _inputArbiter;
    std::unique_ptr<InteractiveStreamFramer> _streamFramer;
    std::unique_ptr<SessionCommandCoordinator> _commandCoordinator;
    bool transition(SessionState next);   ///< 状态机迁移（校验合法性）
    bool beginReconnect(bool automatic = false); ///< 进入重连状态并提交连接请求
    bool _automaticReconnectAttempt{false}; ///< 当前连接是否由自动重连触发
    void startPump();                       ///< 启动输入泵
    void stopPump();                        ///< 停止并销毁输入泵
    void clearAttachment(bool requestDisconnect); ///< 清理传输层附加与信号连接
    void reportError(SessionErrorCategory category, const QString& message,
                     bool retryable = false, int code = 0); ///< 上报结构化错误

    /**
     * @brief 建立传输信号连接，并把 generation 绑进每个处理器。
     * @param transport  目标传输。
     * @param generation 本世代号；处理器只接受与当前 generation 相符的信号。
     */
    void connectTransportSignals(ITransport* transport, quint64 generation);

    /**
     * @brief 按当前 generation 重建传输信号连接。
     * @note 每次 generation 自增（start/beginReconnect）后必须调用，否则
     *       处理器仍持有旧世代号，新连接的信号会被误判为迟到信号丢弃。
     */
    void rewireTransportSignals();

    SessionId _sessionId{QUuid::createUuid()};
    SessionState _state{SessionState::Created};
    RuntimeConfig _config;
    SessionStatistics _statistics;
    std::unique_ptr<TerminalCore> _ownedCore;  ///< 内部拥有的核心（构造时创建）
    TerminalCore* _core{nullptr};               ///< 当前核心（可能借用或自有）
    QPointer<ITransport> _transport;
    SessionInputPump* _inputPump{nullptr};
    QMetaObject::Connection _coreOutputConnection;          ///< 核心输出→传输写入
    QVector<QMetaObject::Connection> _transportConnections; ///< 传输信号连接集合
    /// 最近一次结构化传输错误，供随后的 errorOccurred 取用分类（见 ITransport.h
    /// 中关于两个错误信号发出顺序的约定）。取用后立即清空。
    std::optional<TransportError> _pendingTransportError;
    Ownership _ownership{Ownership::Borrowed};
    bool _acceptsUserInput{true};  ///< 是否接受用户输入（关闭后置 false）
};
