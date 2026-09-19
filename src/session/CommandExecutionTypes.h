/**
 * @file CommandExecutionTypes.h
 * @brief Session 级受限命令执行请求、能力与结构化完成证据。
 */
#pragma once

#include <QByteArray>
#include <QMetaType>

#include <optional>

/** @brief 命令执行通道类型。 */
enum class CommandExecutionMode
{
    Isolated,          ///< 独立通道或独立进程，不污染交互终端。
    InteractiveFramed, ///< 共享交互字节流中的受控 framing 事务。
};

/** @brief 受限命令的结构化执行结果分类。 */
enum class CommandExecutionOutcome
{
    Completed,
    Failed,
    Cancelled,
    TimedOut,
    OutputLimit,
    Disconnected,
};

/** @brief 命令执行的输出与时间预算。 */
struct CommandExecutionLimits
{
    qsizetype maxOutputBytes{65536}; ///< stdout/stderr 合计原始字节预算。
    int timeoutMs{5000};             ///< Executor 执行预算，单位毫秒。
};

/** @brief 由可信策略构造后提交给 Executor 的请求。 */
struct CommandExecutionRequest
{
    quint64 requestId{0};
    QByteArray command;
    CommandExecutionLimits limits;
};

/** @brief Executor 对终止证据与输出隔离能力的声明。 */
struct CommandExecutorCapabilities
{
    CommandExecutionMode mode{CommandExecutionMode::Isolated};
    bool reliableExitCode{false};
    bool reliableTermination{false};
    bool isolatedOutput{false};
};

/** @brief 跨 Executor 统一的有界命令完成证据。 */
struct CommandExecutionResult
{
    quint64 requestId{0};
    quint64 connectionGeneration{0};
    CommandExecutionOutcome outcome{CommandExecutionOutcome::Failed};
    bool executionMayHaveStarted{false};
    bool terminationConfirmed{false};
    bool outputTruncated{false};
    std::optional<int> exitCode;
    QByteArray standardOutput;
    QByteArray standardError;
};
Q_DECLARE_METATYPE(CommandExecutionResult)
