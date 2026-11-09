#pragma once
#include "mcp/protocol/ToolCodec.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

namespace NovaTerm::Mcp::TestSupport {
// 工具说明是 Agent 指引；兼容 fixture 继续锁定名称、参数、结果和注解。
inline QJsonValue withoutToolDescriptions(const QJsonValue& value)
{
    if (value.isArray()) {
        QJsonArray result;
        for (const auto& item : value.toArray()) result.append(withoutToolDescriptions(item));
        return result;
    }
    if (!value.isObject()) return value;
    auto tool = value.toObject();
    tool.remove("description");
    return tool;
}

inline QJsonValue legacyObservationSchema(const QJsonValue& value)
{
    if (value.isArray()) {
        QJsonArray out;
        for (const auto& item : value.toArray()) out.append(legacyObservationSchema(item));
        return out;
    }
    if (!value.isObject()) return value;
    auto object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it)
        it.value() = legacyObservationSchema(it.value());
    auto properties = object.value("properties").toObject();
    if (properties.contains("displayName") && properties.contains("sessionId")) {
        properties.remove("targetLabel"); object.insert("properties", properties);
    }
    if (properties.contains("inputReadiness") && properties.contains("command") && properties.contains("control")) {
        properties.remove("screenSize"); properties.remove("viewportPartial"); properties.remove("lastCommand");
        properties.remove("terminalConfiguration"); properties.remove("ioStatus");
        const QJsonObject open{{"type", "object"}, {"additionalProperties", true}};
        properties.insert("inputReadiness", open); properties.insert("command", open); properties.insert("control", open);
        object.insert("properties", properties);
    }
    if (properties.contains("submissionStatus") && properties.contains("stopRequested")) {
        properties.remove("taskSubmission"); properties.remove("stopSubmission");
        object.insert("properties", properties);
        QJsonArray required;
        for (const auto& name : object.value("required").toArray())
            if (name != "taskSubmission" && name != "stopSubmission") required.append(name);
        object.insert("required", required);
    }
    return object;
}

// Explicit migration of the three changed input contracts; frozen files stay historical.
inline QJsonValue withDirectInputContract(const QJsonValue& value)
{
    if (value.isArray()) {
        QJsonArray result;
        for (const auto& item : value.toArray()) result.append(withDirectInputContract(item));
        return result;
    }
    if (!value.isObject()) return value;
    auto tool = value.toObject();
    const auto name = tool.value("name").toString();
    if (!QStringList{"novaterm_interact_terminal", "novaterm_start_task", "novaterm_request_elevation"}.contains(name)) return tool;
    auto input = tool.value("inputSchema").toObject();
    QJsonArray required;
    for (const auto& field : input.value("required").toArray()) if (field != "inputToken") required.append(field);
    input.insert("required", required);
    auto properties = input.value("properties").toObject();
    auto token = properties.value("inputToken").toObject();
    token.insert("minLength", 1);
    token.insert("description", "Optional legacy observation token. Omit for direct admission after client approval. If supplied, it must remain valid; invalid or expired tokens never fall back to direct admission.");
    properties.insert("inputToken", token); input.insert("properties", properties); tool.insert("inputSchema", input);
    return tool;
}

inline bool retiredTool(const QString& name)
{
    return QStringList{"novaterm_list_commands", "novaterm_execute_command", "novaterm_run_command", "novaterm_run_script",
        "novaterm_list_targets", "novaterm_open_target", "novaterm_query_target", "novaterm_close_target",
        "novaterm_list_file_resources", "novaterm_list_serial_file_targets"}.contains(name);
}
inline QJsonObject toolByName(const QString& name)
{
    for (const auto& value : ToolCodec::catalog())
        if (value.toObject().value("name") == name) {
            auto tool = value.toObject();
            // Project the separately tested approval envelope out of frozen business fixtures.
            // ContractsTests::approvalPresentationCatalog checks the unprojected catalog.
            tool.remove("title");
            auto annotations = tool.value("annotations").toObject();
            annotations.remove("title"); tool.insert("annotations", annotations);
            auto input = tool.value("inputSchema").toObject();
            auto properties = input.value("properties").toObject();
            for (const auto* field : {"1_操作", "2_目标", "3_内容"}) properties.remove(field);
            QJsonArray required;
            for (const auto& field : input.value("required").toArray())
                if (field != "1_操作" && field != "2_目标" && field != "3_内容") required.append(field);
            input.insert("required", required);
            input.insert("properties", properties); tool.insert("inputSchema", input);
            return tool;
        }
    return {};
}
inline QJsonValue retainedSchema(const QJsonValue& value)
{
    if (value.isObject()) {
        auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (it.key() == "enum") {
                QJsonArray values;
                for (const auto& item : it.value().toArray())
                    if (!QStringList{"list_commands", "execute_command", "run_command", "run_script", "human_confirmation"}.contains(item.toString())) values.append(item);
                it.value() = values;
            } else it.value() = retainedSchema(it.value());
        }
        return object;
    }
    if (value.isArray()) {
        QJsonArray items;
        for (const auto& item : value.toArray()) items.append(retainedSchema(item));
        return items;
    }
    return value;
}

}
