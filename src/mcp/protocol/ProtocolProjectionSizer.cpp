/** @file ProtocolProjectionSizer.cpp @brief 观察结果编码预算测量实现。 */
#include "ProtocolProjectionSizer.h"
#include "mcp/McpProtocol.h"

#include <QJsonDocument>

namespace NovaTerm::Mcp {

qsizetype ProtocolProjectionSizer::encodedToolResultBytes(const QJsonObject& data)
{
    return QJsonDocument(toolResult(success(data))).toJson(QJsonDocument::Compact).size();
}

} // namespace NovaTerm::Mcp
