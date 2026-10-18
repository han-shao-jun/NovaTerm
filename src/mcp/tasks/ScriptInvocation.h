/**
 * @file ScriptInvocation.h
 * @brief 脚本发布后写入终端的调用命令构造；校验与执行共用同一实现。
 */
#pragma once

#include <QString>

#include <optional>

namespace NovaTerm::Mcp {

/**
 * @brief 按目标 Shell 类型生成脚本调用命令。
 * @param shellKind `posix`、`powershell` 或 `cmd`。
 * @param workingDirectory 非空工作目录；含 NUL/CR/LF 时拒绝。
 * @param invocation 已解析的脚本调用文本。
 * @return 可写入终端的命令；参数不安全或 Shell 未知时为空。
 */
[[nodiscard]] std::optional<QString> scriptInvocationCommand(
    const QString& shellKind, const QString& workingDirectory,
    const QString& invocation);

} // namespace NovaTerm::Mcp
