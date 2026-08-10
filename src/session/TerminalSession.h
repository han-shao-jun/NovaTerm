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
#include "transport/ITransport.h"

#include <QObject>
#include <QPointer>
#include <QVector>
#include <memory>

class SessionInputPump;
class TerminalCore;

/**
 * @brief 终端会话：一个终端运行时 + 一条传输连接。
 *
 * 管理 TerminalCore 与 ITransport 的生命周期、信号连接与状态机迁移。
 * 不持有任何 UI/渲染资源，可被 SessionManager 批量管理或独立使用。
 */
class TerminalSession final : public QObject
{
    Q_OBJECT
public:
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
    /** @brief 传输层指针（未附加时为 nullptr）。 */
    [[nodiscard]] ITransport* transport() const { return _transport.data(); }

    /**
     * @brief 附加传输层。
     * @param transport 传输层。
     * @param ownership 所有权模式（Adopt 表示会话接管其 deleteLater）。
     * @note 会先清理已有附加，再建立信号连接并启动输入泵。
     */
    void attach(ITransport* transport, Ownership ownership = Ownership::Adopt);

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
     * @brief 重连（Running/Failed 状态可调用）。
     * @return true 表示已请求重连；状态不合法返回 false。
     */
    [[nodiscard]] bool reconnect();

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
    bool transition(SessionState next);   ///< 状态机迁移（校验合法性）
    void startPump();                       ///< 启动输入泵
    void stopPump();                        ///< 停止并销毁输入泵
    void clearAttachment(bool requestDisconnect); ///< 清理传输层附加与信号连接
    void reportError(SessionErrorCategory category, const QString& message,
                     bool retryable = false, int code = 0); ///< 上报结构化错误

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
    Ownership _ownership{Ownership::Borrowed};
    bool _acceptsUserInput{true};  ///< 是否接受用户输入（关闭后置 false）
};
