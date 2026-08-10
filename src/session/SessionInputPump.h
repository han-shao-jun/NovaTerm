/**
 * @file   SessionInputPump.h
 * @brief  会话输入泵：Transport → TerminalCore 的有界字节通路。
 *
 * 拥有 Transport 到 TerminalCore 的字节流转送逻辑，施加背压控制：
 * 当解析队列接近上限时暂停 Transport 读取，队列恢复后再 drain。
 * 解析器（TerminalCore）始终是唯一写入方；GUI 仅提交字节并观察过载诊断。
 */
#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>

class ITransport;
class TerminalCore;

/**
 * @brief 会话输入泵：在 Transport 与 TerminalCore 之间做背压转送。
 *
 * 监听 ITransport::readyRead，将字节分块喂给 TerminalCore::writeInput；
 * 当解析队列满时调用 setReadPaused(true) 暂停读取，队列恢复后自动 drain。
 * 待处理字节上限为 MaxPendingBytes，超限触发 overload 信号。
 */
class SessionInputPump final : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief 输入泵运行统计。
     */
    struct Statistics
    {
        quint64 receivedBytes{0};   ///< 累计从 Transport 收到的字节
        quint64 acceptedBytes{0};   ///< 累计被解析器接受的字节
        qsizetype pendingBytes{0};  ///< 当前待处理字节数
        quint64 pauseCount{0};      ///< 触发暂停的次数
        quint64 overloadCount{0};   ///< 过载次数
    };

    /**
     * @brief 构造输入泵。
     * @param transport 传输层（提供 readyRead 信号与 setReadPaused）。
     * @param core       终端核心（提供 writeInput 与背压信号）。
     * @param parent    父对象。
     */
    SessionInputPump(ITransport* transport, TerminalCore* core,
                     QObject* parent = nullptr);
    ~SessionInputPump() override;

    /**
     * @brief 启动泵：连接 Transport/Core 信号，开始转送。
     */
    void start();

    /**
     * @brief 停止泵：暂停读取、断开信号、清空待处理缓冲。
     */
    void stop();

    /**
     * @brief 获取当前统计快照。
     * @return 统计结构（pendingBytes 反映实时待处理量）。
     */
    [[nodiscard]] Statistics statistics() const;

signals:
    /**
     * @brief 输入过载（待处理超限或 Transport 不支持暂停）。
     * @param reason 过载原因。
     */
    void overload(const QString& reason);

private:
    void acceptBytes(const QByteArray& data);  ///< 接收并转送一段字节
    void handleBackpressure(bool paused);       ///< 响应解析器背压状态变化
    void drainPending();                         ///< 排空待处理缓冲
    void reportOverload(const QString& reason); ///< 上报过载并暂停读取

    static constexpr qsizetype MaxPendingBytes = 8 * 1024 * 1024; ///< 待处理上限 8 MiB
    static constexpr qsizetype InputChunkBytes = 64 * 1024;       ///< 单次喂入块 64 KiB

    QPointer<ITransport> _transport;
    QPointer<TerminalCore> _core;
    QByteArray _pending;       ///< 解析器满时缓存的待处理字节
    bool _running{false};
    Statistics _statistics;
};
