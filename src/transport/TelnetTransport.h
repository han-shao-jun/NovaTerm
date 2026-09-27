/**
 * @file   TelnetTransport.h
 * @brief  Telnet 字节传输。
 *
 * 基于 QTcpSocket 的事件驱动 Telnet 传输实现，协议状态机由 libtelnet
 * 承担（IAC 转义、RFC 1143 Q-method 选项协商、SB/SE 子协商）。不包含
 * 终端模型知识——仅在字节流层面工作，净数据经 readyRead() 上抛。
 *
 * 与 SshTransport 不同，本实现不需要工作线程：QTcpSocket 本身异步，
 * 全部逻辑运行在 GUI 线程的信号槽中。
 *
 * 支持的能力：暂停读取（背压）、NAWS 窗口尺寸上报、IAC NOP 保活、重连。
 */
#pragma once

#include "ITransport.h"
#include "session/SessionTypes.h"

#include <QAbstractSocket>
#include <QTcpSocket>
#include <QTimer>

#include <memory>

class TelnetTransport final : public ITransport
{
    Q_OBJECT
public:
    explicit TelnetTransport(TelnetConfig config, QObject* parent = nullptr);
    ~TelnetTransport() override;

    /**
     * @brief 建立 TCP 连接并启动 Telnet 协商。
     * @return true 表示配置有效且连接请求已提交（异步建立）；
     *         false 表示配置无效，连接未启动。
     * @note 协商在 TCP 连接建立后由 libtelnet 自动进行，connected()
     *       在 TCP 层就绪时即发出，不等待选项协商完成。
     */
    bool connectToHost() override;

    /**
     * @brief 关闭连接、释放协议状态机并发出 disconnected() 信号。
     */
    void disconnect() override;

    /**
     * @brief 写入数据到远端。
     * @param data 待发送的字节数据。
     * @note 经 telnet_send_text() 发出：0xFF 自动加倍转义；未协商 BINARY 时
     *       按 RFC 854 将 CR 转为 CR NUL、LF 转为 CR LF，协商成功后透传。
     *       内部限制待写队列不超过 1 MiB，超限会报错。
     */
    void write(const QByteArray& data) override;

    /**
     * @brief 上报终端尺寸到远端（NAWS 子协商）。
     * @param cols 列数（必须 >0）。
     * @param rows 行数（必须 >0）。
     * @note 尺寸始终缓存；仅在配置启用 NAWS 且对端已回 DO NAWS 后才实际
     *       发送报文。对端拒绝 NAWS 时不伪装成功（与 SerialTransport 一致）。
     */
    void resizeTerminal(int cols, int rows) override;

    /**
     * @brief 查询 TCP 连接是否已建立。
     * @return true 表示 socket 处于已连接状态。
     */
    [[nodiscard]] bool isConnected() const override;

    /**
     * @brief 获取最近一次错误的描述。
     * @return 错误字符串（无错误时为空）。
     */
    [[nodiscard]] QString errorString() const override;

    /**
     * @brief 暂停或恢复读取对端数据。
     * @param paused true 暂停读取，false 恢复并立即 drain 已缓冲数据。
     * @return 始终为 true（Telnet 支持背压）。
     * @note Telnet 没有 SSH 那样的应用层流控窗口，停止 drain socket 即等于
     *       让 TCP 接收窗口收缩、由对端自然减速，不存在协议死锁风险。
     */
    bool setReadPaused(bool paused) override;

    /**
     * @brief 查询本实现支持的能力位。
     * @return PauseReads | ResizeTerminal | KeepAlive | Reconnect。
     */
    [[nodiscard]] TransportCapabilities capabilities() const override
    {
        return TransportCapability::PauseReads
            | TransportCapability::ResizeTerminal
            | TransportCapability::KeepAlive
            | TransportCapability::Reconnect;
    }

    /**
     * @brief 获取构造时的 Telnet 配置。
     * @return 配置常引用。
     */
    [[nodiscard]] const TelnetConfig& config() const noexcept { return _config; }

    /**
     * @brief 对端是否已接受 NAWS（DO NAWS）。
     * @return true 表示窗口尺寸上报生效。
     */
    [[nodiscard]] bool nawsNegotiated() const noexcept;

    /**
     * @brief 是否已协商 BINARY 发送方向。
     * @return true 表示出站按 8 位透传，不做 NVT CR 转换。
     */
    [[nodiscard]] bool binaryNegotiated() const noexcept;

private:
    void handleSocketConnected();       ///< TCP 就绪：创建协议状态机并起保活
    void handleSocketDisconnected();    ///< 对端关闭连接
    void handleSocketError(QAbstractSocket::SocketError error); ///< socket 错误映射
    void readAvailable();               ///< drain socket 并喂给 libtelnet
    void sendKeepAlive();               ///< 发送 IAC NOP
    void writeRaw(const char* data, qsizetype size); ///< libtelnet 出站字节直写 socket
    void teardown();                    ///< 释放协议状态机、停表、关 socket
    void reportError(TransportErrorCategory category, int code,
                     const QString& message, bool retryable);

    static constexpr qint64 MaxPendingWriteBytes = 1024 * 1024;   ///< 写队列上限 1 MiB
    /// socket 读缓冲上限 1 MiB。缓冲只在暂停 drain 时占用；缩小的只是
    /// TCP 窗口收缩前的缓冲深度（吞吐由消费速度决定，不受此影响）。
    static constexpr int ReadBufferBytes = 1024 * 1024;
    static constexpr int ReadChunkBytes = 64 * 1024;              ///< 单次 drain 粒度

    // PImpl 模式隔离 libtelnet 头文件依赖（与 VTAdapter 对 libvterm 的做法一致）。
    class Impl;
    std::unique_ptr<Impl> _impl;

    TelnetConfig _config;
    QTcpSocket _socket;
    QTimer _keepAliveTimer;
    QString _errorString;
    int _columns{0};                ///< 最近一次上报的列数（0 表示未知）
    int _rows{0};                   ///< 最近一次上报的行数（0 表示未知）
    bool _readPaused{false};        ///< 是否暂停读取
    bool _disconnectEmitted{false}; ///< 是否已发出 disconnected()（避免重复）
};
