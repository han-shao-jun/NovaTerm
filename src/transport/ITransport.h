/**
 * @file   ITransport.h
 * @brief  字节传输抽象接口。
 *
 * 定义终端与外部系统通信的统一抽象接口，支持串口、SSH、本地 shell 等
 * 多种后端。所有实现必须与终端模型解耦——仅处理字节流与连接状态管理，
 * 不依赖终端 cell、渲染器、控件或序列化配置。上层通过 capabilities()
 * 查询能力位，据此决定是否调用 resizeTerminal()/setReadPaused() 等可选接口。
 */
#pragma once

#include <QByteArray>
#include <QFlags>
#include <QMetaType>
#include <QObject>
#include <QString>

/**
 * @brief 传输能力位标志。
 *
 * 实现通过 capabilities() 返回支持的能力组合，上层据此决定可调用的
 * 可选接口。未声明的能力对应接口调用为空实现。
 */
enum class TransportCapability : quint32
{
    None = 0,                       ///< 无任何可选能力
    PauseReads = 1U << 0,           ///< 支持暂停读取（背压控制）
    ResizeTerminal = 1U << 1,       ///< 支持调整远程终端尺寸
    KeepAlive = 1U << 2,            ///< 支持发送保活包
    Reconnect = 1U << 3,            ///< 支持断线重连
};
Q_DECLARE_FLAGS(TransportCapabilities, TransportCapability)
Q_DECLARE_OPERATORS_FOR_FLAGS(TransportCapabilities)

/**
 * @brief 传输错误分类。
 *
 * 用于结构化错误上报，便于上层按类别决定重试策略或用户提示。
 */
enum class TransportErrorCategory
{
    Configuration,  ///< 配置无效（端口名、参数等）
    Resolve,        ///< 主机名解析失败
    Connection,     ///< 连接建立失败
    Authentication, ///< 认证失败
    HostKey,        ///< 主机密钥校验失败
    Permission,     ///< 权限不足（如打开设备被拒）
    Protocol,       ///< 协议层错误
    Io,             ///< 读写 I/O 错误
    Overload,       ///< 内部队列过载
    Unknown,        ///< 未知错误
};

/**
 * @brief 结构化传输错误。
 *
 * 携带错误分类、平台/库相关错误码、人类可读消息，以及是否建议重试。
 * 通过 transportError() 信号投递，供上层做统一错误处理。
 */
struct TransportError
{
    TransportErrorCategory category{TransportErrorCategory::Unknown}; ///< 错误分类
    int code{0};            ///< 平台/库相关错误码
    QString message;        ///< 人类可读的错误描述
    bool retryable{false};  ///< 是否建议重试
};
Q_DECLARE_METATYPE(TransportError)

/**
 * @brief 传输会话退出原因。
 *
 * 区分正常退出、失败退出、崩溃、用户主动关闭等场景，便于上层
 * 决定是否自动重连或提示用户。通过 exited() 信号投递。
 */
enum class TransportExitReason
{
    NormalExit,   ///< 子进程/会话正常退出
    FailedExit,   ///< 子进程以非零状态退出
    Crash,        ///< 子进程崩溃（信号终止）
    UserClosed,   ///< 用户主动关闭连接
    IoError,      ///< I/O 错误导致退出
    StartFailed,  ///< 启动失败
};
Q_DECLARE_METATYPE(TransportExitReason)

/**
 * @brief 字节流异步传输契约。
 *
 * 所有传输后端（串口/SSH/本地 shell）实现本接口。实现不得依赖终端
 * cell、渲染器、控件或序列化配置——仅处理字节流与连接状态管理。
 * 信号一律在 GUI 线程发出（实现负责跨线程投递）。
 */
class ITransport : public QObject
{
    Q_OBJECT
public:
    explicit ITransport(QObject* parent = nullptr) : QObject(parent) {}
    ~ITransport() override = default;

    /**
     * @brief 建立连接。
     * @return true 表示连接请求已成功提交（异步建立）；
     *         false 表示配置无效，连接未启动。
     * @note 实际连接结果通过 connected()/errorOccurred() 信号通知。
     */
    virtual bool connectToHost() = 0;

    /**
     * @brief 异步建立连接（默认等同 connectToHost()）。
     * @return 同 connectToHost()。
     */
    virtual bool connectAsync() { return connectToHost(); }

    /**
     * @brief 断开连接并释放底层资源。
     *
     * 调用后应保证不再发出信号，已缓冲的写入会尽力刷新但无保证。
     */
    virtual void disconnect() = 0;

    /**
     * @brief 写入数据到传输通道。
     * @param data 待发送的字节数据（不为空）。
     * @note 实现需保证线程安全，内部应使用队列缓冲避免阻塞调用线程。
     *       实际发送进度通过 bytesWritten() 信号反馈。
     */
    virtual void write(const QByteArray& data) = 0;

    /**
     * @brief 调整远程终端尺寸（PTY 列/行）。
     * @param columns 列数（必须 >0）。
     * @param rows    行数（必须 >0）。
     * @note 仅具备 ResizeTerminal 能力的实现有效；连接前调用会缓存为初始尺寸。
     */
    virtual void resizeTerminal(int columns, int rows) = 0;

    /**
     * @brief 查询当前是否已连接。
     * @return true 表示连接已建立且可用。
     */
    [[nodiscard]] virtual bool isConnected() const = 0;

    /**
     * @brief 是否存在待完成的断开操作。
     * @return true 表示已请求断开但尚未完成清理（如工作线程未回收）。
     */
    [[nodiscard]] virtual bool hasPendingDisconnect() const { return false; }

    /**
     * @brief 暂停或恢复读取对端数据（背压控制）。
     * @param paused true 暂停读取，false 恢复读取。
     * @return true 表示能力被支持并已设置；false 表示不支持。
     * @note 暂停时实现通常仍需轮询底层协议以避免流控死锁，仅停止向上层投递数据。
     */
    virtual bool setReadPaused(bool paused)
    {
        Q_UNUSED(paused);
        return false;
    }

    /**
     * @brief 查询本实现支持的能力位组合。
     * @return TransportCapabilities 标志位。
     */
    [[nodiscard]] virtual TransportCapabilities capabilities() const
    {
        return TransportCapability::None;
    }

    /**
     * @brief 获取最近一次错误的描述。
     * @return 错误字符串（无错误时为空）。
     */
    [[nodiscard]] virtual QString errorString() const = 0;

signals:
    /**
     * @brief 连接已成功建立。
     */
    void connected();

    /**
     * @brief 连接已断开（主动或对端关闭）。
     */
    void disconnected();

    /**
     * @brief 收到对端数据。
     * @param data 收到的字节流。
     */
    void readyRead(const QByteArray& data);

    /**
     * @brief 数据已写入底层通道。
     * @param bytes 本次已写入的字节数。
     */
    void bytesWritten(qint64 bytes);

    /**
     * @brief 发生错误。
     * @param error 人类可读的错误描述。
     */
    void errorOccurred(const QString& error);

    /**
     * @brief 发生结构化传输错误。
     * @param error 携带分类、错误码、可重试标志的错误对象。
     */
    void transportError(const TransportError& error);

    /**
     * @brief 子进程/会话已退出。
     * @param exitCode 退出码。
     * @param reason   退出原因分类。
     */
    void exited(quint32 exitCode, TransportExitReason reason);
};
