/**
 * @file Job.h
 * @brief RPC 请求的运行时任务记录；由任务组件持有，Service 只通过键访问。
 */
#pragma once

#include "mcp/contracts/ToolRequests.h"
#include "TargetReservation.h"

#include <QJsonObject>
#include <QString>

#include <atomic>
#include <memory>

namespace NovaTerm::Mcp {

struct Job
{
    QString connection;
    QString id;
    QString tool;
    /** @brief 脚本确认摘要（接收时由 wire 参数计算，用于重放复核）。 */
    QByteArray confirmationDigest;
    /** @brief 脚本元数据摘要（不含正文，用于发布结果核对）。 */
    QByteArray scriptMetadataHash;
    /** @brief 脚本发布后解析出的调用命令。 */
    QString resolvedCommand;
    /** @brief 脚本已发布但尚未提交终端执行；用于给拒绝结果补充发布事实。 */
    bool scriptPublished{false};
    /** @brief wire 校验后解码的强类型请求；新代码只读它。 */
    ToolRequest request;
    QString requestState;
    QJsonObject inputResponses;
    qint64 deadline{0};
    qint64 created{0};
    bool confirmationAccepted{false};
    bool mrtr{false};
    bool formElicitation{false};
    QString scriptPolicyVersion;
    std::unique_ptr<TargetReservation> reservation;
    std::atomic<bool> cancelled{false};
};

} // namespace NovaTerm::Mcp
