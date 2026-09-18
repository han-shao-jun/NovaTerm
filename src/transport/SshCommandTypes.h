/**
 * @file SshCommandTypes.h
 * @brief 辅助 SSH 命令的有界请求及结构化完成证据，不暴露 libssh 类型。
 */
#pragma once
#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <optional>

enum class SshCommandOutcome { Completed, Failed, Cancelled, TimedOut, OutputLimit, Disconnected };

struct SshCommandLimits
{
    qsizetype maxOutputBytes{65536}; ///< stdout/stderr 合计原始字节预算
    int timeoutMs{5000};
};

struct SshCommandResult
{
    quint64 requestId{0};
    quint64 connectionGeneration{0};
    SshCommandOutcome outcome{SshCommandOutcome::Failed};
    bool executionMayHaveStarted{false};
    bool terminationConfirmed{false};
    bool outputTruncated{false};
    std::optional<int> exitCode;
    QByteArray standardOutput;
    QByteArray standardError;
};
Q_DECLARE_METATYPE(SshCommandResult)
