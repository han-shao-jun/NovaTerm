/**
 * @file ProtocolProjectionSizer.h
 * @brief 在序列化边界测量观察结果的真实编码大小。
 *
 * 领域投影按该反馈有界调整，而不是在领域模型里创建 QJsonObject 估算大小。
 * 测量对象与 wire 最终发送的 tool result 一致（含 text 与 structuredContent）。
 */
#pragma once

#include <QJsonObject>
#include <QtGlobal>

namespace NovaTerm::Mcp {

class ProtocolProjectionSizer
{
public:
    ProtocolProjectionSizer() = delete;

    /** @brief 以 `data` 为成功载荷测量最终 tool result 的紧凑 JSON 字节数。 */
    [[nodiscard]] static qsizetype encodedToolResultBytes(const QJsonObject& data);
};

} // namespace NovaTerm::Mcp
