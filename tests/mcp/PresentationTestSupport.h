#pragma once
#include "mcp/protocol/ToolCodec.h"
#include <QJsonObject>

namespace NovaTerm::Mcp::TestSupport {
// Business fixtures explicitly supply the new wire envelope. Dedicated raw
// presentation tests deliberately bypass these helpers to verify rejection.
inline QJsonObject withToolPresentation(const QString& name, QJsonObject arguments)
{
    for (const auto& value : ToolCodec::catalog()) {
        const auto tool = value.toObject();
        if (tool.value("name") != name) continue;
        if (!arguments.contains("1_操作")) arguments.insert("1_操作", tool.value("title"));
        if (!arguments.contains("2_目标")) arguments.insert("2_目标", "测试终端");
        if (!arguments.contains("3_内容")) arguments.insert("3_内容", "测试请求");
        break;
    }
    return arguments;
}
inline QJsonObject presentedRpc(QJsonObject message)
{
    if (message.value("method") != "tools/call" || !message.value("params").isObject()) return message;
    auto params = message.value("params").toObject();
    if (params.contains("arguments") && !params.value("arguments").isObject()) return message;
    params.insert("arguments", withToolPresentation(params.value("name").toString(), params.value("arguments").toObject()));
    message.insert("params", params);
    return message;
}
inline QString validateBusinessArguments(const QString& name, const QJsonObject& args)
{ return ToolCodec::validate(name, withToolPresentation(name, args)); }
inline auto decodeBusinessRequest(const QString& name, const QJsonObject& args)
{ return ToolCodec::decode(name, withToolPresentation(name, args)); }
inline auto decodeBusinessCall(const QString& name, const QJsonObject& args)
{ return ToolCodec::decodeCall(name, withToolPresentation(name, args)); }
}
