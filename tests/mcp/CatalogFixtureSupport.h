#pragma once
#include "mcp/protocol/ToolCodec.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

namespace NovaTerm::Mcp::TestSupport {
inline bool retiredTool(const QString& name)
{
    return QStringList{"novaterm_list_commands", "novaterm_execute_command", "novaterm_run_command", "novaterm_run_script",
        "novaterm_list_targets", "novaterm_open_target", "novaterm_query_target", "novaterm_close_target",
        "novaterm_list_file_resources", "novaterm_list_serial_file_targets"}.contains(name);
}
inline QJsonObject toolByName(const QString& name)
{
    for (const auto& value : ToolCodec::catalog())
        if (value.toObject().value("name") == name) return value.toObject();
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
