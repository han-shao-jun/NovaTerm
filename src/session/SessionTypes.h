/**
 * @file   SessionTypes.h
 * @brief  会话层公共类型定义。
 *
 * 集中声明会话状态、错误分类、传输配置快照与运行时元数据等跨模块共享类型。
 * 所有配置结构（SerialConfig/SshConfig/LocalShellConfig）均为不可变创建快照，
 * 由 UI/Profile 层构造、由 transport 层消费，二者之间不传递 widget 或 JSON。
 * 敏感信息（密码、私钥口令）仅存在于构造快照中，绝不持久化。
 */
#pragma once

#include <QMetaType>
#include <QDateTime>
#include <QSerialPort>
#include <QString>
#include <QVariantMap>
#include <QUuid>

/// 会话唯一标识，使用 UUID 便于跨进程持久化与恢复。
using SessionId = QUuid;

/**
 * @brief 会话生命周期状态。
 *
 * 状态机迁移合法性由 TerminalSession::transition() 校验，非法迁移会被拒绝并告警。
 */
enum class SessionState {
    Created,       ///< 已创建，尚未连接
    Connecting,    ///< 连接建立中
    Running,       ///< 已连接且正在运行
    Reconnecting,  ///< 重连中（断线后或用户主动重连）
    Failed,        ///< 连接/会话失败（可重试）
    Closing,       ///< 正在关闭（等待传输层清理）
    Closed,        ///< 已完全关闭，可被回收或复用
};
Q_DECLARE_METATYPE(SessionState)

/**
 * @brief 会话错误分类。
 *
 * 用于结构化错误上报，便于上层按类别决定重试策略或用户提示。
 */
enum class SessionErrorCategory {
    None,            ///< 无错误
    Configuration,   ///< 配置无效
    Connection,      ///< 连接建立失败
    Authentication,  ///< 认证失败
    HostKey,         ///< 主机密钥校验失败
    Permission,      ///< 权限不足
    Protocol,        ///< 协议层错误
    InputOverload,   ///< 输入解析队列过载
    Io,              ///< 读写 I/O 错误
    Internal,        ///< 内部逻辑错误
};

/**
 * @brief 结构化会话错误。
 *
 * 携带错误分类、平台/库相关错误码、人类可读消息，以及是否建议重试。
 * 通过 sessionError() 信号投递。
 */
struct SessionError
{
    SessionErrorCategory category{SessionErrorCategory::None}; ///< 错误分类
    int code{0};            ///< 平台/库相关错误码
    QString message;        ///< 人类可读的错误描述
    bool retryable{false};  ///< 是否建议重试
};
Q_DECLARE_METATYPE(SessionError)

/**
 * @brief 关闭模式。
 */
enum class CloseMode { Graceful, Abort };
Q_DECLARE_METATYPE(CloseMode)

/**
 * @brief 传输后端类型。
 */
enum class TransportKind { LocalShell, Ssh, Serial, Telnet, Custom };
Q_DECLARE_METATYPE(TransportKind)

/**
 * @brief 运行时会话配置。
 *
 * 描述一个已解析、可直接创建会话的配置快照。transport 子图为 QVariantMap，
 * 由 SessionFactory 按 transportKind 反序列化为具体 Config 结构。
 */
struct RuntimeConfig
{
    static constexpr int CurrentSchemaVersion = 1; ///< 配置 schema 版本，用于未来迁移

    int schemaVersion{CurrentSchemaVersion};
    QString profileId;                ///< 关联的 profile 标识（可空）
    QString credentialRef;           ///< 凭据引用（可空，用于 SSH 密码/口令）
    QString title;                   ///< 会话显示标题
    TransportKind transportKind{TransportKind::LocalShell}; ///< 传输后端类型
    QVariantMap transport;           ///< 传输子配置（按 transportKind 解释）
    QVariantMap presentationDefaults; ///< 展示层默认值（颜色/字体等）
};
Q_DECLARE_METATYPE(RuntimeConfig)

/**
 * @brief 会话运行统计。
 *
 * 用于 UI 展示流量、重连次数、会话时长等信息。generation 每次重连自增，
 * 用于区分不同连接世代的信号（避免迟到的旧世代信号干扰新连接）。
 */
struct SessionStatistics
{
    quint64 bytesReceived{0};   ///< 累计接收字节
    quint64 bytesSent{0};      ///< 累计发送字节
    quint64 reconnectCount{0};  ///< 重连次数
    quint64 generation{0};      ///< 连接世代号（每次连接/重连自增）
    QDateTime createdAt{QDateTime::currentDateTimeUtc()}; ///< 会话创建时间（UTC）
    QDateTime connectedAt;      ///< 最近一次连接建立时间（UTC）
};

/**
 * @brief 会话恢复元数据。
 *
 * 持久化到 SessionStore 的最小信息，用于应用重启后恢复会话列表。
 * 不包含敏感数据（密码/口令），恢复时通过 credentialRef 重新解析凭据。
 */
struct SessionRestoreMetadata
{
    SessionId sessionId;             ///< 会话标识
    QString profileId;               ///< 关联 profile
    QVariantMap overrides;           ///< 传输子配置覆盖项
    RuntimeConfig runtimeSnapshot;   ///< 运行时配置快照
    bool reconnectOnRestore{true};    ///< 恢复后是否自动重连
};
Q_DECLARE_METATYPE(SessionRestoreMetadata)

/**
 * @brief 串口会话不可变创建快照。
 *
 * 由 UI/Profile 层构造，transport 层消费。transport 不读取 widget 或 JSON，
 * 仅消费此结构。帧格式配置在构造前解析完成。
 */
struct SerialConfig
{
    QString portName;                                        ///< 串口名（如 COM3、/dev/ttyUSB0）
    qint32 baudRate{115200};                                 ///< 波特率
    QSerialPort::DataBits dataBits{QSerialPort::Data8};      ///< 数据位
    QSerialPort::Parity parity{QSerialPort::NoParity};       ///< 校验位
    QSerialPort::StopBits stopBits{QSerialPort::OneStop};    ///< 停止位
    QSerialPort::FlowControl flowControl{QSerialPort::NoFlowControl}; ///< 流控
    QString label;                                           ///< 显示标签

    /**
     * @brief 校验配置是否有效。
     * @return true 表示端口名非空且所有帧格式参数取值合法。
     */
    [[nodiscard]] bool isValid() const
    {
        const bool validDataBits = dataBits == QSerialPort::Data5
            || dataBits == QSerialPort::Data6 || dataBits == QSerialPort::Data7
            || dataBits == QSerialPort::Data8;
        const bool validParity = parity == QSerialPort::NoParity
            || parity == QSerialPort::EvenParity || parity == QSerialPort::OddParity
            || parity == QSerialPort::SpaceParity || parity == QSerialPort::MarkParity;
        const bool validStopBits = stopBits == QSerialPort::OneStop
            || stopBits == QSerialPort::OneAndHalfStop
            || stopBits == QSerialPort::TwoStop;
        const bool validFlowControl = flowControl == QSerialPort::NoFlowControl
            || flowControl == QSerialPort::HardwareControl
            || flowControl == QSerialPort::SoftwareControl;
        return !portName.trimmed().isEmpty() && baudRate > 0 && validDataBits
            && validParity && validStopBits && validFlowControl;
    }
};

Q_DECLARE_METATYPE(SerialConfig)

/**
 * @brief SSH 会话不可变创建快照。
 *
 * 由 UI/Profile 层构造，transport 层消费。密码与私钥口令仅存在于本快照中，
 * 绝不持久化到日志或配置文件；凭据通过 credentialRef 间接引用。
 */
struct SshConfig
{
    QString host;        ///< 主机名或 IP
    QString username;    ///< 登录用户名
    quint16 port{22};    ///< SSH 端口

    QString authMethod{QStringLiteral("password")}; ///< 认证方式："password" 或 "publickey"
    QString password;            ///< 密码（仅 authMethod == "password" 时有效）
    QString privateKeyPath;      ///< 私钥路径（仅 authMethod == "publickey" 时有效）
    QString keyPassphrase;       ///< 私钥口令（可选，用于解密私钥）

    QString terminalType{QStringLiteral("xterm-256color")}; ///< TERM 环境变量值
    int keepAliveSeconds{30};    ///< 保活间隔秒数（0 表示禁用）
    QString label;               ///< 显示标签

    /**
     * @brief 校验配置是否有效。
     * @return true 表示主机/用户名非空、端口非零，且认证所需字段齐全。
     */
    [[nodiscard]] bool isValid() const
    {
        if (host.trimmed().isEmpty() || username.trimmed().isEmpty())
            return false;
        if (port == 0)
            return false;
        if (authMethod == QStringLiteral("password"))
            return !password.isEmpty();
        if (authMethod == QStringLiteral("publickey"))
            return !privateKeyPath.trimmed().isEmpty();
        return false;
    }
};

Q_DECLARE_METATYPE(SshConfig)

/**
 * @brief 主机密钥校验状态。
 *
 * 首次信任或主机密钥变更时由 transport 上报 UI 决策，绝不静默接受。
 */
enum class SshHostKeyStatus { New, Changed };

/**
 * @brief 主机密钥信息（提交给 UI 决策）。
 *
 * transport 计算此结构时不依赖任何 UI；由对话框决定接受或拒绝。
 */
struct SshHostKeyInfo
{
    QString host;        ///< 主机名
    quint16 port{0};     ///< 端口
    QString keyType;        ///< 密钥类型（如 "ssh-ed25519"）
    QString fingerprint;    ///< SHA-256 指纹（冒号分隔的十六进制）
    SshHostKeyStatus status{SshHostKeyStatus::New}; ///< 新主机或密钥变更
};

Q_DECLARE_METATYPE(SshHostKeyInfo)
