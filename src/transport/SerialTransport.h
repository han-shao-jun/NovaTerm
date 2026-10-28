/**
 * @file   SerialTransport.h
 * @brief  串口字节传输。
 *
 * 基于 QtSerialPort 的事件驱动串口传输实现，不包含终端模型知识；
 * 帧格式配置（波特率/数据位/校验/停止位/流控）需在构造前解析完成。
 * 独立 I/O 线程拥有端口，GUI 门面支持暂停交付（背压）与重连；
 * 物理串口无远程 PTY 概念，
 * 故不支持终端尺寸调整（resizeTerminal() 为空实现）。
 */
#pragma once

#include "ITransport.h"
#include "session/SessionTypes.h"
#include "SerialIoState.h"

#include <QSerialPort>
#include <QByteArrayView>

class SerialTransport final : public ITransport
{
    Q_OBJECT
public:
    explicit SerialTransport(SerialConfig config, QObject* parent = nullptr);
    ~SerialTransport() override;

    /**
     * @brief 打开串口并建立连接。
     * @return true 表示已接受异步打开请求；false 表示配置无效。
     * @note 实际打开在 I/O 线程执行，结果通过 GUI 排队通知发布。
     *       驱动拒绝线路参数也按 Configuration 类错误上报。线路参数在
     *       打开后会再下发一次：端口关闭时 setter 只缓存取值，真正
     *       触达驱动的校验发生在 open() 内部，此时被拒即关闭端口并
     *       报错，不会留下 isConnected() 为真而线路参数不匹配的状态。
     */
    bool connectToHost() override;

    /**
     * @brief 关闭串口并发出 disconnected() 信号。
     */
    void disconnect() override;

    /**
     * @brief 写入数据到串口。
     * @param data 待发送的字节数据。
     * @note 内部限制待写队列不超过 1 MiB，超限会报错。
     */
    void write(const QByteArray& data) override;
    /** @brief 有界部分写：返回接受前缀、0 队列满、-1 连接/I/O 错误。
     * 非零expectedGeneration在I/O队列锁内复核，旧代际拒绝写入。
     */
    [[nodiscard]] qint64 tryWriteBounded(QByteArrayView data, qint64 limit = 16 * 1024, quint64 expectedGeneration = 0);
    /** @brief 出件箱与底层待写总量；清写未确认时另含屏障哨兵，非零旧代际返回-1。 */
    [[nodiscard]] qint64 pendingWriteBytes(quint64 expectedGeneration = 0) const;
    /**
     * @brief 请求丢弃未提交的输出；返回请求是否接受，不阻塞等待 I/O。
     * @note 已提交底层的有限前缀不可撤回；pendingWriteBytes() 在清写
     *       屏障确认前不返回零，新写入只能在此屏障之后提交。
     *       非零expectedGeneration在I/O队列锁内复核，旧代际不清新队列。
     */
    [[nodiscard]] bool clearPendingOutput(quint64 expectedGeneration = 0);

    /**
     * @brief 调整终端尺寸（串口无此能力，空实现）。
     * @param cols 列数（忽略）。
     * @param rows 行数（忽略）。
     */
    void resizeTerminal(int cols, int rows) override;

    /**
     * @brief 查询串口是否已打开。
     * @return true 表示串口已打开可读写。
     */
    [[nodiscard]] bool isConnected() const override;
    [[nodiscard]] bool hasPendingDisconnect() const override;
    /** @brief 有界收发、底层完成与驱动容量的线程安全统计。 */
    [[nodiscard]] NovaTerm::SerialIoSnapshot ioStatistics() const;
    /** @brief 当前进程仍在运行的串口 I/O worker 数，供生命周期诊断。 */
    [[nodiscard]] static int activeIoWorkerCount() noexcept;

    /**
     * @brief 获取最近一次错误的描述。
     * @return 错误字符串（无错误时为空）。
     */
    [[nodiscard]] QString errorString() const override;

    /**
     * @brief 暂停或恢复向 GUI 消费者交付数据。
     * @param paused true 暂停交付，false 恢复并排队交付缓冲数据。
     * @return 始终为 true。
     * @note I/O 继续排空端口到 8 MiB 有界收件箱，满后停止排空；
     *       无流控链路不能保证任意长停顿无损。
     */
    bool setReadPaused(bool paused) override;

    /**
     * @brief 查询本实现支持的能力位。
     * @return PauseReads | Reconnect。
     */
    [[nodiscard]] TransportCapabilities capabilities() const override
    {
        return TransportCapability::PauseReads
            | TransportCapability::Reconnect;
    }

    /**
     * @brief 获取构造时的串口配置。
     * @return 配置常引用。
     */
    [[nodiscard]] const SerialConfig& config() const noexcept { return _config; }

private:
    friend class SerialTransportIoTestAccess;
#ifdef NOVATERM_SERIAL_STRESS
    // 仅压力夹具在打开前设置队列实验请求，生产目标不定义此宏。
    friend class SerialStressPortAccess;
    void setDriverReceiveQueueForTesting(quint32 bytes);
#endif
    class Impl;
    void deliverInput();                       ///< GUI 每次最多交付 256 KiB
    void deliverNotices();                     ///< GUI 合并发布状态与写完成
    void wakeIo();                             ///< 最多一个待处理控制唤醒
    Q_SIGNAL void ioWakeRequested();            ///< 仅连接内部 I/O worker
    /// 记录错误并先发 transportError、再发 errorOccurred（顺序约定见 ITransport.h）。
    void reportError(const QString& message,
                     TransportErrorCategory category = TransportErrorCategory::Io,
                     bool retryable = false);

    static constexpr qint64 MaxPendingWriteBytes = 1024 * 1024; ///< 写队列上限 1 MiB

    SerialConfig _config;
    std::unique_ptr<Impl> _impl;
    std::shared_ptr<NovaTerm::SerialIoState> _state; ///< 与 worker 同一份交接状态
    QString _errorString;
    bool _connectPending{false};    ///< 是否有待完成的异步打开
    bool _disconnectEmitted{false}; ///< 是否已发出 disconnected()（避免重复）
    bool _connected{false}; ///< 仅由 GUI 发布状态改变
};
