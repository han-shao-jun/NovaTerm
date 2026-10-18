/**
 * @file CapabilityAdapter.h
 * @brief 运行时把 Session 事实转换成纯应用层 `CapabilityFacts`。
 *
 * 这是 Session 与应用能力模型之间的唯一适配点：连接类型、Profile 与终端
 * 控制器分支只在这里出现，界面和业务查询不重复这些规则。
 */
#pragma once

#include "mcp/application/CapabilityResolver.h"
#include "session/SessionDirectory.h"

namespace NovaTerm::Mcp {

class AccessStore;

[[nodiscard]] CapabilityFacts capabilityFacts(const SessionDirectory::Entry& entry,
                                              const AccessStore& access);

/** @brief 解析当前客户端在该会话上的统一能力视图。 */
[[nodiscard]] CapabilitySnapshot sessionCapabilities(const SessionDirectory::Entry& entry,
    const QString& clientId, const AccessStore& access, bool humanConfirmation);

} // namespace NovaTerm::Mcp
