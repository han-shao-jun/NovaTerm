/** @file ToolPresentation.h
 * Required approval presentation, never execution or authorization data.
 * Codex 0.166 sorts argument names and displays three values, each truncated to
 * 60 graphemes. Numeric prefixes keep the three readable fields before business parameters.
 */
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <array>

namespace NovaTerm::Mcp::Presentation {
struct Definition {
    const char* name;
    const char* title;
    const char* action;
    const char* detail;
};
inline constexpr std::array Definitions{
    Definition{"novaterm_list_sessions", "查看已连接终端", "查看已连接终端", "State the requested list size; do not imply opening a connection."},
    Definition{"novaterm_read_context", "读取目标终端", "读取终端：<targetLabel: ssh · IP or serial port>", "Say current screen, recent output, committed history, or expanded detail according to source/start/expandToken. Mention waiting only if requested."},
    Definition{"novaterm_search_context", "搜索终端输出", "搜索终端：<targetLabel: ssh · IP or serial port>", "Show the literal query and screen/recent-output scope."},
    Definition{"novaterm_interact_terminal", "操作目标终端", "Describe the actual action: run command, send text, paste, press key, submit line, or release control", "Show the literal command/text prefix or key names (e.g. Ctrl+C); for multiple actions include action count and submission. Release control does not stop a program."},
    Definition{"novaterm_request_elevation", "请求管理员权限", "管理员操作：<sudo or su> <command or shell>", "Show the command prefix, or explicitly say enter administrator shell. Never include credentials."},
    Definition{"novaterm_list_captures", "查看历史录制", "查看已有历史录制", "State the list size; this does not start recording."},
    Definition{"novaterm_read_history", "读取历史录制", "读取录制：<known recording name or captureId>", "Say read recorded output and the requested byte limit; distinguish recordings from live terminals."},
    Definition{"novaterm_search_history", "搜索历史录制", "搜索录制：<known recording name or captureId>", "Show the query prefix and text/raw-bytes mode; do not decode arbitrary bytes into executable text."},
    Definition{"novaterm_clear_terminal", "清空终端内容", "清空终端：<targetLabel: ssh · IP or serial port>", "Explicitly distinguish scrollback only from screen AND scrollback. Does not stop remote programs or delete recordings."},
    Definition{"novaterm_wait_text", "等待终端输出", "等待终端：<targetLabel: ssh · IP or serial port>", "Show expected literal texts (prefix plus count if many), any/all matching, screen/log source and wait duration."},
    Definition{"novaterm_start_task", "启动终端任务", "启动任务：<literal command or input prefix>", "Show additional action count/submission and timeout; do not claim completion."},
    Definition{"novaterm_query_task", "查看任务进度", "查看任务：<known command prefix or taskId/operationId>", "Say read status/output; no rerun. Use only previously observed task context."},
    Definition{"novaterm_stop_task", "请求中断任务", "请求中断：<known command prefix or taskId>", "Say send Ctrl+C; remote termination is not guaranteed."},
    Definition{"novaterm_upload_file", "上传文件到终端", "上传 <source filename>; explicitly say overwrite or no overwrite", "Show localPath -> remotePath; shorten middle of long paths, keep filename and direction. Upload does not execute the file."},
    Definition{"novaterm_download_file", "下载终端文件", "下载 <source filename>; explicitly say overwrite or no overwrite", "Show remotePath -> localPath; shorten middle of long paths, keep filename and direction. Local path is on the NovaTerm host."},
    Definition{"novaterm_query_transfer", "查看文件传输进度", "查看传输：<known filename or transferId>", "Show known upload/download direction and paths; no resume or rerun."},
    Definition{"novaterm_cancel_transfer", "取消文件传输", "请求取消传输：<known filename or transferId>", "Show known direction and destination. Cancellation does not undo an already published file."},
    Definition{"novaterm_upload_serial_file", "串口发送文件", "串口发送 <source filename>; overwrite is controlled by the receiver, not this tool", "Show localPath -> remoteName (defaults to source filename) via ZMODEM; remote directory is determined by receiver, never invent it."},
    Definition{"novaterm_query_serial_transfer", "查看串口传输进度", "查看串口传输：<known filename or transferId>", "Say read ZMODEM progress, not proof of remote publication."},
    Definition{"novaterm_cancel_serial_transfer", "取消串口文件传输", "请求取消串口发送：<known filename or transferId>", "Show known filename and serial target; cancellation does not guarantee remote file removal."},
};

inline bool isField(const QString& field)
{
    return field == "1_操作" || field == "2_目标" || field == "3_内容";
}
inline QString missingFieldsMessage(const QJsonObject& arguments)
{
    for (const auto* field : {"1_操作", "2_目标", "3_内容"}) {
        const auto name = QString::fromUtf8(field);
        if (!arguments.contains(name))
            return QStringLiteral("缺少确认信息“%1”。请重新连接 NovaTerm MCP 以刷新工具定义，并填写 1_操作、2_目标、3_内容。本次未执行任何操作。").arg(name);
    }
    return {};
}
inline bool validValue(const QJsonValue& value)
{
    if (!value.isString()) return false;
    const auto text = value.toString();
    if (text.trimmed().isEmpty() || text.size() > 120) return false;
    int characters = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const auto c = text[i];
        char32_t scalar = c.unicode();
        if (c.isHighSurrogate()) {
            if (++i >= text.size() || !text[i].isLowSurrogate()) return false;
            scalar = QChar::surrogateToUcs4(c, text[i]);
        } else if (c.isLowSurrogate()) return false;
        const auto category = QChar::category(scalar);
        if (category == QChar::Other_Control || category == QChar::Other_Format
            || category == QChar::Separator_Line || category == QChar::Separator_Paragraph)
            return false;
        if (++characters > 60) return false;
    }
    return true;
}
inline QJsonObject decorate(QJsonObject tool)
{
    const auto name = tool.value("name").toString();
    for (const auto& definition : Definitions) {
        if (name != QLatin1String(definition.name)) continue;
        const auto title = QString::fromUtf8(definition.title);
        tool.insert("title", title);
        auto annotations = tool.value("annotations").toObject();
        annotations.insert("title", title);
        tool.insert("annotations", annotations);
        auto input = tool.value("inputSchema").toObject();
        auto properties = input.value("properties").toObject();
        const auto field = [](const QString& description) {
            return QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 60},
                {"description", "Required approval display, not authorization. Single line, at most 60 Unicode characters; aim for 54. Use Chinese for explanatory text, preserve literal commands/paths and mark any shortening with an ellipsis. " + description}};
        };
        auto action = field("Copy the fixed operation name exactly.");
        action.insert("const", title);
        properties.insert("1_操作", action);
        properties.insert("3_内容", field(QString::fromUtf8(definition.action) + "; " + QString::fromUtf8(definition.detail)));
        properties.insert("2_目标", field("Copy targetLabel from list_sessions for the selected session: SSH must say ssh · concrete IP, serial must say the actual port (e.g. /dev/ttyUSB0 or COM3). Do not use displayName, nicknames or device-role names. If IP is unavailable, explicitly say so; never turn a hostname into an invented IP. For recordings use known capture name/ID. For task/transfer queries use previously observed context or the exact ID. Never guess or include tokens/passwords. For list tools say NovaTerm connected terminals/existing recordings."));
        auto required = input.value("required").toArray();
        for (const auto* name : {"1_操作", "2_目标", "3_内容"})
            if (!required.contains(name)) required.append(name);
        input.insert("required", required);
        input.insert("properties", properties);
        tool.insert("inputSchema", input);
        break;
    }
    return tool;
}
} // namespace NovaTerm::Mcp::Presentation
