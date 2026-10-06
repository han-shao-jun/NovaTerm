/**
 * @file SerialFileTransferController.h
 * @brief Session 持有的串口协议/文件门面，不持有 UI 或渲染资源。
 */
#pragma once
#include "SerialTransferTypes.h"
#include <QByteArray>
#include <QByteArrayView>
#include <QObject>
#include <functional>
#include <memory>

class SerialFileTransferController final : public QObject {
    Q_OBJECT
public:
    /** @brief 由 Session 安装的字节通道；所有回调仅在本对象线程调用。 */
    struct Channel {
        std::function<bool()> connected;
        std::function<bool(quint64)> reserve; ///< 抢占前拒绝已有 MCP Lease。
        std::function<void()> activate; ///< Core 旧输出和普通待写已排空后锁住所有输入。
        std::function<void()> release;
        std::function<qint64(QByteArrayView)> write; ///< 返回接受长度、0 背压、-1 错误。
        std::function<qint64()> pendingWriteBytes;
        std::function<bool()> clearWrites;
        std::function<bool()> coreIdle; ///< 非阻塞查询旧 Parser 命令与普通 pending。
        int baudRate{115200};
        int writeDrainTimeoutMs{0}; ///< 0 根据波特率推导宿主输出停滞期限。
        bool eightDataBits{true};
        bool softwareFlowControl{false};
    };
    explicit SerialFileTransferController(Channel channel, QObject* parent = nullptr);
    ~SerialFileTransferController() override;
    /** @brief 提交异步传输，false 时可读取 errorString；活动任务不可重入开始。 */
    [[nodiscard]] bool start(const SerialTransferRequest& request);
    [[nodiscard]] bool isActive() const;
    [[nodiscard]] QString errorString() const;
    [[nodiscard]] SerialTransferProgress progress() const;
    /** @brief 从唯一 InputPump 入口取得未改写的协议字节。 */
    void acceptBytes(const QByteArray& data);
    /** @brief 字节通道已可继续写；不得将信号计数直接当作设备确认。 */
    void notifyWritable();
    /** @brief 用户取消；已启动协议时清理输出并发送取消后要求断开链路。 */
    void cancel();
    /** @brief 断连、重绑、关闭时无网络写入的立即失效路径。 */
    void abort(const QString& reason);
signals:
    void activeChanged(bool active);
    void progressChanged(const SerialTransferProgress& progress);
    void readPauseChanged(bool paused);
    void visibleRemainder(const QByteArray& bytes);
    void finished(bool success, const QString& message);
    void disconnectRequired();
private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
