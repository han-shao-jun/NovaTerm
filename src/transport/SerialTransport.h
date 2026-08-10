/**
 * @file   SerialTransport.h
 * @brief  串口字节传输。
 *
 * 基于 QtSerialPort 的事件驱动串口传输实现，不包含终端模型知识；
 * 帧格式配置（波特率/数据位/校验/停止位/流控）需在构造前解析完成。
 * 支持暂停读取（背压）与重连能力；物理串口无远程 PTY 概念，
 * 故不支持终端尺寸调整（resizeTerminal() 为空实现）。
 */
#pragma once

#include "ITransport.h"
#include "session/SessionTypes.h"

#include <QSerialPort>

class SerialTransport final : public ITransport
{
    Q_OBJECT
public:
    explicit SerialTransport(SerialConfig config, QObject* parent = nullptr);
    ~SerialTransport() override;

    /**
     * @brief 打开串口并建立连接。
     * @return true 表示串口配置有效且已异步打开；false 表示配置无效。
     * @note 实际打开在 QueuedConnection 中执行，避免在会话创建栈上
     *       重入地发出 connected()/errorOccurred() 信号。
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

    /**
     * @brief 获取最近一次错误的描述。
     * @return 错误字符串（无错误时为空）。
     */
    [[nodiscard]] QString errorString() const override;

    /**
     * @brief 暂停或恢复读取对端数据。
     * @param paused true 暂停读取，false 恢复并立即尝试读取缓冲数据。
     * @return 始终为 true（串口支持背压）。
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
    void readAvailable();                       ///< 串口有数据可读时 drain
    void handleError(QSerialPort::SerialPortError error); ///< 处理串口错误
    void reportError(const QString& message);   ///< 记录并发出 errorOccurred()

    static constexpr qint64 MaxPendingWriteBytes = 1024 * 1024; ///< 写队列上限 1 MiB

    SerialConfig _config;
    QSerialPort _port;
    QString _errorString;
    bool _readPaused{false};        ///< 是否暂停读取
    bool _connectPending{false};    ///< 是否有待完成的异步打开
    bool _disconnectEmitted{false}; ///< 是否已发出 disconnected()（避免重复）
};
