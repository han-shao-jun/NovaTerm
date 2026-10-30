/**
 * @file ContractsTests.cpp
 * @brief MCP 契约与解码边界：目录兼容、参数接受/拒绝、强类型 ID、错误编码。
 */
#include "mcp/contracts/Errors.h"
#include "capabilities/CapabilityRegistry.h"
#include "mcp/contracts/Identifiers.h"
#include "mcp/protocol/ToolCodec.h"
#include "mcp/protocol/ProtocolProjectionSizer.h"
#include "mcp/McpProtocol.h"

#include <QFile>
#include "CatalogFixtureSupport.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <cstddef>
#include <type_traits>
#include <tuple>

using namespace NovaTerm::Mcp;
using namespace NovaTerm::Mcp::TestSupport;

namespace {
const QString SessionUuid = QStringLiteral("11111111-1111-4111-8111-111111111111");
const QString OperationUuid = QStringLiteral("22222222-2222-4222-8222-222222222222");

#ifdef Q_OS_WIN
const QString LocalFilePath = QStringLiteral("C:/build/firmware.bin");
#else
const QString LocalFilePath = QStringLiteral("/home/test/build/firmware.bin");
#endif
QJsonObject transferResultSchema(const QJsonObject& tool)
{
    const auto data = tool.value("outputSchema").toObject().value("properties").toObject().value("data").toObject();
    return data.contains("oneOf") ? data.value("oneOf").toArray().first().toObject() : data;
}

QJsonObject identity()
{
    return {{QStringLiteral("sessionId"), SessionUuid},
        {QStringLiteral("epoch"), QStringLiteral("epoch-1")}};
}

QJsonObject argumentsFor(const QString& name)
{
    QJsonObject arguments = identity();
    if (name == QLatin1String("novaterm_list_sessions"))
        return {};
    if (name == QLatin1String("novaterm_search_context")) {
        arguments.insert(QStringLiteral("captureId"), QStringLiteral("capture-1"));
        arguments.insert(QStringLiteral("query"), QStringLiteral("hello"));
    } else if (name == QLatin1String("novaterm_execute_command")) {
        arguments.insert(QStringLiteral("commandId"), QStringLiteral("os.uname"));
        arguments.insert(QStringLiteral("policyVersion"), QStringLiteral("policy-1"));
        arguments.insert(QStringLiteral("commandTicket"), QStringLiteral("ticket"));
        arguments.insert(QStringLiteral("arguments"), QJsonObject{});
    } else if (name == QLatin1String("novaterm_run_command")) {
        arguments.insert(QStringLiteral("command"), QStringLiteral("uname -a"));
    } else if (name == QLatin1String("novaterm_run_script")) {
        arguments.insert(QStringLiteral("scriptContent"), QStringLiteral("#!/bin/sh\ntrue\n"));
        arguments.insert(QStringLiteral("targetPath"), QStringLiteral("/tmp/novaterm.sh"));
        arguments.insert(QStringLiteral("workingDirectory"), QStringLiteral("/tmp"));
        arguments.insert(QStringLiteral("invocation"), QStringLiteral("/tmp/novaterm.sh"));
    } else if (name == QLatin1String("novaterm_interact_terminal")) {
        arguments.insert(QStringLiteral("inputToken"), QStringLiteral("input"));
        arguments.insert(QStringLiteral("operationId"), OperationUuid);
        arguments.insert(QStringLiteral("actions"),
            QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("line_ending")}}});
    } else if (name == QLatin1String("novaterm_request_elevation")) {
        arguments.insert(QStringLiteral("inputToken"), QStringLiteral("input"));
        arguments.insert(QStringLiteral("operationId"), OperationUuid);
        arguments.insert(QStringLiteral("method"), QStringLiteral("sudo"));
        arguments.insert(QStringLiteral("mode"), QStringLiteral("shell"));
    }
    return arguments;
}

const QStringList& toolNames()
{
    static const QStringList names{
        QStringLiteral("novaterm_list_sessions"),
        QStringLiteral("novaterm_read_context"),
        QStringLiteral("novaterm_search_context"),
        QStringLiteral("novaterm_interact_terminal"),
        QStringLiteral("novaterm_request_elevation"),
    };
    return names;
}
} // namespace

class ContractsTests : public QObject
{
    Q_OBJECT
private slots:
    void directFilePathsDoNotRequireSharedDirectories_data()
    {
        QTest::addColumn<QString>("name");
        for (const auto* name : {"novaterm_upload_file", "novaterm_download_file", "novaterm_upload_serial_file"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void directFilePathsDoNotRequireSharedDirectories()
    {
        QFETCH(QString, name);
        auto args = identity();
        args.insert("operationId", OperationUuid);
#ifdef Q_OS_WIN
        args.insert("localPath", "C:/build/firmware.bin");
#else
        args.insert("localPath", "/home/test/build/firmware.bin");
#endif
        if (name != "novaterm_upload_serial_file") args.insert("remotePath", "/home/test/firmware.bin");
        const auto decoded = ToolCodec::decodeCall(name, args);
        QVERIFY2(!isError(decoded), qPrintable(ToolCodec::validate(name, args)));
    }
    void retiredCommandToolsAreRejected_data()
    {
        QTest::addColumn<QString>("name");
        for (const auto* name : {"novaterm_list_commands", "novaterm_execute_command",
                 "novaterm_run_command", "novaterm_run_script", "novaterm_list_targets",
                 "novaterm_open_target", "novaterm_query_target", "novaterm_close_target",
                 "novaterm_list_file_resources", "novaterm_list_serial_file_targets"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void retiredCommandToolsAreRejected()
    {
        QFETCH(QString, name);
        QVERIFY(!ToolCodec::isKnownTool(name));
        QVERIFY(!ToolCodec::validate(name, argumentsFor(name)).isEmpty());
        QVERIFY(isError(ToolCodec::decodeCall(name, argumentsFor(name))));
        const auto registry = NovaTerm::Capabilities::CapabilityRegistry::implementedCapabilities();
        QVERIFY(!registry.findByMcpTool(name));
    }
    void fileOverwriteIsAnOptionalBoolean_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("upload") << QString("novaterm_upload_file");
        QTest::newRow("download") << QString("novaterm_download_file");
    }
    void fileOverwriteIsAnOptionalBoolean()
    {
        QFETCH(QString, name);
        QJsonObject args{{"sessionId", SessionUuid}, {"epoch", "epoch-1"}, {"localPath", LocalFilePath},
            {"remotePath", "/tmp/result.bin"}, {"operationId", OperationUuid}};
        QVERIFY(ToolCodec::validate(name, args).isEmpty());
        args.insert("overwrite", true); QVERIFY(ToolCodec::validate(name, args).isEmpty());
        const auto decoded = ToolCodec::decodeCall(name, args); QVERIFY(!isError(decoded));
        const auto& transfer = std::get<FileTransferRequest>(std::get<ToolCall>(decoded));
        QVERIFY(name == "novaterm_upload_file" ? std::get<StartFileUploadRequest>(transfer).overwrite
            : std::get<StartFileDownloadRequest>(transfer).overwrite);
        args.insert("overwrite", false); QVERIFY(ToolCodec::validate(name, args).isEmpty());
        args.remove("overwrite");
        const auto defaultCall = ToolCodec::decodeCall(name, args); QVERIFY(!isError(defaultCall));
        const auto& defaults = std::get<FileTransferRequest>(std::get<ToolCall>(defaultCall));
        QVERIFY(name == "novaterm_upload_file" ? !std::get<StartFileUploadRequest>(defaults).overwrite
            : !std::get<StartFileDownloadRequest>(defaults).overwrite);
        for (const auto& value : QJsonArray{"true", 1, QJsonValue::Null, QJsonObject{}}) {
            args.insert("overwrite", value); QVERIFY(!ToolCodec::validate(name, args).isEmpty());
        }
        QFile fixture(QStringLiteral(NOVATERM_SOURCE_DIR "/tests/mcp/fixtures/file-overwrite-tools-2026-10-28.json"));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        const auto expected = QJsonDocument::fromJson(fixture.readAll()).array(); QCOMPARE(expected.size(), 2);
        const auto tool = toolByName(name);
        // The path/confirmation input changed; transfer facts remain frozen.
        QCOMPARE(transferResultSchema(tool), transferResultSchema(expected.at(name == "novaterm_upload_file" ? 0 : 1).toObject()));
        QCOMPARE(tool.value("inputSchema").toObject().value("properties").toObject().value("overwrite"),
            expected.at(name == "novaterm_upload_file" ? 0 : 1).toObject().value("inputSchema").toObject().value("properties").toObject().value("overwrite"));
        QVERIFY(!tool.value("inputSchema").toObject().value("required").toArray().contains("overwrite"));
    }
    void directTransferPathsAndLegacyFields_data()
    {
        directFilePathsDoNotRequireSharedDirectories_data();
    }
    void directTransferPathsAndLegacyFields()
    {
        QFETCH(QString, name);
        QJsonObject args{{"sessionId", SessionUuid}, {"epoch", "epoch-1"}, {"localPath", LocalFilePath}, {"operationId", OperationUuid}};
        const bool serial = name == "novaterm_upload_serial_file";
        if (!serial) args.insert("remotePath", "/tmp/firmware.bin");
        const auto decoded = ToolCodec::decodeCall(name, args); QVERIFY(!isError(decoded));
        QCOMPARE(ToolCodec::toolName(std::get<ToolCall>(decoded)), name);
        for (const auto& field : args.keys()) {
            auto missing = args; missing.remove(field);
            QVERIFY2(isError(ToolCodec::decodeCall(name, missing)), qPrintable(field));
        }
        for (const auto& field : QStringList{"resourceId", "resourceRevision", "relativePath", "scopeId", "scopeRevision",
                "targetId", "targetRevision", "clientId", "host", "portName", "password"}) {
            auto invalid = args; invalid.insert(field, "injected");
            QVERIFY2(isError(ToolCodec::decodeCall(name, invalid)), qPrintable(field));
        }
        for (const auto& field : QStringList{"sessionId", "operationId"}) {
            auto invalid = args; invalid.insert(field, "not-a-uuid");
            QVERIFY(isError(ToolCodec::decodeCall(name, invalid)));
        }
        for (const auto& path : QStringList{"", "relative.bin", "../escape", "./file", "//server/share/file",
                LocalFilePath + "/../other", LocalFilePath + "/", LocalFilePath + ":stream",
                LocalFilePath + "\n", LocalFilePath + QChar(0x85), LocalFilePath + QString(256, 'a')}) {
            auto invalid = args; invalid.insert("localPath", path);
            QVERIFY2(isError(ToolCodec::decodeCall(name, invalid)), qPrintable(path));
        }
        if (!serial) {
            for (const auto& path : QStringList{"", "relative.bin", "/", "/a/../b", "/a//b", "/a/", "/name:stream", "/a\\b", "/a\nb"}) {
                auto invalid = args; invalid.insert("remotePath", path);
                QVERIFY2(isError(ToolCodec::decodeCall(name, invalid)), qPrintable(path));
            }
            args.insert("remotePath", "/firmware.bin");
            QVERIFY(!isError(ToolCodec::decodeCall(name, args)));
        } else {
            const auto& request = std::get<StartSerialFileUploadRequest>(std::get<SerialFileTransferRequest>(std::get<ToolCall>(decoded)));
            QCOMPARE(request.localPath, LocalFilePath); QCOMPARE(request.remoteName, QString("firmware.bin"));
            for (const auto& remoteName : QStringList{"", ".", "..", "a/b", "a\\b", "name:stream", "a\nb", QString(256, 'a'), QString(86, QChar(0x4e00))}) {
                auto invalid = args; invalid.insert("remoteName", remoteName);
                QVERIFY2(isError(ToolCodec::decodeCall(name, invalid)), qPrintable(remoteName));
            }
        }
#ifdef Q_OS_WIN
        args.insert("localPath", "C:/firmware.bin");
#else
        args.insert("localPath", "/firmware.bin");
#endif
        QVERIFY(!isError(ToolCodec::decodeCall(name, args)));
    }
    void uploadsRequirePairedConfirmationParameters_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("sftp") << QString("novaterm_upload_file");
        QTest::newRow("uart") << QString("novaterm_upload_serial_file");
    }
    void uploadsRequirePairedConfirmationParameters()
    {
        QFETCH(QString, name);
        QJsonObject args{{"sessionId", SessionUuid}, {"epoch", "epoch-1"}, {"localPath", LocalFilePath}, {"operationId", OperationUuid}};
        if (name == "novaterm_upload_file") args.insert("remotePath", "/tmp/firmware.bin");
        QVERIFY(!isError(ToolCodec::decodeCall(name, args)));
        auto invalid = args; invalid.insert("confirmed", true); QVERIFY(isError(ToolCodec::decodeCall(name, invalid)));
        invalid = args; invalid.insert("confirmationToken", "opaque"); QVERIFY(isError(ToolCodec::decodeCall(name, invalid)));
        args.insert("confirmationToken", "opaque");
        for (const bool accept : {false, true}) {
            args.insert("confirmed", accept);
            const auto decoded = ToolCodec::decodeCall(name, args); QVERIFY(!isError(decoded));
            if (name == "novaterm_upload_file") {
                const auto& upload = std::get<StartFileUploadRequest>(std::get<FileTransferRequest>(std::get<ToolCall>(decoded)));
                QCOMPARE(upload.confirmationToken, QString("opaque")); QVERIFY(upload.confirmed); QCOMPARE(*upload.confirmed, accept);
            } else {
                const auto& upload = std::get<StartSerialFileUploadRequest>(std::get<SerialFileTransferRequest>(std::get<ToolCall>(decoded)));
                QCOMPARE(upload.confirmationToken, QString("opaque")); QVERIFY(upload.confirmed); QCOMPARE(*upload.confirmed, accept);
            }
        }
        for (const auto& bad : QJsonArray{QJsonValue::Null, "true", 1, QJsonObject{}}) {
            invalid = args; invalid.insert("confirmed", bad); QVERIFY(isError(ToolCodec::decodeCall(name, invalid)));
        }
        for (const auto& bad : QJsonArray{"", QString(65, 'a'), QJsonValue::Null, true}) {
            invalid = args; invalid.insert("confirmationToken", bad); QVERIFY(isError(ToolCodec::decodeCall(name, invalid)));
        }
        const auto data = toolByName(name).value("outputSchema").toObject().value("properties").toObject().value("data").toObject();
        QCOMPARE(data.value("oneOf").toArray().size(), 2);
        const auto confirmation = data.value("oneOf").toArray().last().toObject();
        const QStringList fields{"instanceId", "state", "sessionId", "epoch", "operationId", "protocol", "localPath",
            "remotePath", "target", "overwrite", "confirmationToken", "expiresInMs", "message"};
        QCOMPARE(confirmation.value("properties").toObject().size(), fields.size());
        QVERIFY(!confirmation.value("additionalProperties").toBool(true));
        for (const auto& field : fields) QVERIFY(confirmation.value("required").toArray().contains(field));
        QCOMPARE(confirmation.value("properties").toObject().value("state").toObject().value("enum").toArray(),
            QJsonArray({"confirmation_required", "declined"}));
        QVERIFY(!confirmation.value("properties").toObject().contains("transferId"));
    }
    void downloadUsesSharedTransferContract()
    {
        QFile fixture(QStringLiteral(NOVATERM_SOURCE_DIR "/tests/mcp/fixtures/file-download-tool-2026-10-28.json"));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        const auto frozen = QJsonDocument::fromJson(fixture.readAll()).object();
        const auto tool = toolByName("novaterm_download_file");
        QCOMPARE(tool.value("outputSchema"), frozen.value("outputSchema"));
        QCOMPARE(tool.value("annotations"), frozen.value("annotations"));
        const auto input = tool.value("inputSchema").toObject().value("properties").toObject();
        QVERIFY(!input.contains("confirmed")); QVERIFY(!input.contains("confirmationToken"));
        QVERIFY(!tool.value("description").toString().contains("separate local write"));
    }
    void serialUploadToolsKeepConfirmationAndPublicationSeparate()
    {
        QCOMPARE(ToolCodec::catalog().size(), 20);
        QFile fixture(QStringLiteral(NOVATERM_SOURCE_DIR "/tests/mcp/fixtures/serial-transfer-tools-2026-10-28.json"));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        const auto expected = QJsonDocument::fromJson(fixture.readAll()).array(); QCOMPARE(expected.size(), 3);
        for (const auto& frozen : expected) {
            const auto old = frozen.toObject(); const auto name = old.value("name").toString();
            const auto tool = toolByName(name);
            if (name != "novaterm_upload_serial_file") QCOMPARE(tool, old);
            QCOMPARE(transferResultSchema(tool), transferResultSchema(old));
            const auto data = transferResultSchema(tool);
            QVERIFY(!data.value("additionalProperties").toBool(true));
            QCOMPARE(data.value("properties").toObject().value("remotePublication").toObject().value("const"), QJsonValue("unknown"));
            QCOMPARE(data.value("properties").toObject().value("contentVerified").toObject().value("const"), QJsonValue(false));
        }
    }
    void fileTransferResultSchemaIsClosedAndComplete()
    {
        QCOMPARE(ToolCodec::catalog().size(), 20);
        QFile fixture(QStringLiteral(NOVATERM_SOURCE_DIR "/tests/mcp/fixtures/file-transfer-tools-2026-10-28.json"));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        const auto expected = QJsonDocument::fromJson(fixture.readAll()).array(); QCOMPARE(expected.size(), 3);
        for (const auto& frozen : expected) {
            auto old = frozen.toObject(); const auto name = old.value("name").toString();
            const auto tool = toolByName(name);
            if (name == "novaterm_upload_file") {
                QCOMPARE(transferResultSchema(tool), transferResultSchema(old));
                continue;
            }
            // Preserve the prior download-state extension while comparing every unchanged field.
            auto output = old.value("outputSchema").toObject();
            auto envelope = output.value("properties").toObject(); auto data = envelope.value("data").toObject();
            auto fields = data.value("properties").toObject(); auto state = fields.value("state").toObject();
            auto values = state.value("enum").toArray(); QVERIFY(!values.contains("downloading"));
            values.insert(3, "downloading"); state.insert("enum", values); fields.insert("state", state);
            data.insert("properties", fields); envelope.insert("data", data); output.insert("properties", envelope);
            old.insert("outputSchema", output); QCOMPARE(tool, old);
            QVERIFY(ToolCodec::validate(name, {{"transferId", OperationUuid}}).isEmpty());
            QVERIFY(isError(ToolCodec::decodeCall(name, {{"transferId", "invalid"}})));
            QVERIFY(!ToolCodec::validate(name, {{"transferId", OperationUuid}, {"clientId", "other"}}).isEmpty());
        }
    }
    void directUploadGuidanceExplainsConfirmationAndControlHandoff()
    {
        const auto sftp = toolByName("novaterm_upload_file").value("description").toString();
        const auto serial = toolByName("novaterm_upload_serial_file").value("description").toString();
        for (const auto& description : {sftp, serial}) {
            QVERIFY(description.contains("confirmation_required"));
            QVERIFY(description.contains("Never auto-confirm"));
            QVERIFY(description.contains("operationId"));
        }
        QVERIFY(serial.contains("ZSKIP")); QVERIFY(serial.contains("automatically"));
        QVERIFY(serial.contains("Prepare the receiver separately"));
        QVERIFY(!serial.contains("explicitly release"));
    }
    void workflowCatalogMatchesFrozenFixture()
    {
        QFile fixture(QStringLiteral(NOVATERM_SOURCE_DIR "/tests/mcp/fixtures/workflow-tools-2026-10-27.json"));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        QJsonArray workflow;
        for (const auto* name : {"novaterm_wait_text", "novaterm_start_task", "novaterm_query_task", "novaterm_stop_task"})
            workflow.append(toolByName(QString::fromLatin1(name)));
        QCOMPARE(workflow.size(), 4);
        QCOMPARE(workflow, QJsonDocument::fromJson(fixture.readAll()).array());
    }
    void workflowToolsAreBoundedAndIndependent()
    {
        QCOMPARE(errorCodeFromName("TASK_NOT_AVAILABLE"), std::optional<ErrorCode>{ErrorCode::TaskNotAvailable});
        auto wait = identity(); wait.insert("source", "log");
        wait.insert("texts", QJsonArray{"PASS", QString::fromUtf8("完成")});
        QVERIFY(ToolCodec::isKnownTool("novaterm_wait_text"));
        QVERIFY(ToolCodec::validate("novaterm_wait_text", wait).isEmpty());
        QVERIFY(!isError(ToolCodec::decodeCall("novaterm_wait_text", wait)));
        for (const auto& field : QStringList{"regex", "path", "principal"}) {
            auto invalid = wait; invalid.insert(field, "x");
            QVERIFY(!ToolCodec::validate("novaterm_wait_text", invalid).isEmpty());
        }
        auto invalid = wait; invalid.insert("waitMs", 30001);
        QVERIFY(!ToolCodec::validate("novaterm_wait_text", invalid).isEmpty());
        invalid = wait; invalid.insert("scanBytes", 1048577);
        QVERIFY(!ToolCodec::validate("novaterm_wait_text", invalid).isEmpty());
        invalid = wait; invalid.insert("sinceToken", "a"); invalid.insert("resumeToken", "b");
        QVERIFY(!ToolCodec::validate("novaterm_wait_text", invalid).isEmpty());
        invalid = wait; invalid.insert("source", "screen");
        QVERIFY(!ToolCodec::validate("novaterm_wait_text", invalid).isEmpty());
        invalid.insert("start", "current");
        QVERIFY(ToolCodec::validate("novaterm_wait_text", invalid).isEmpty());
        invalid.insert("sinceToken", "a");
        QVERIFY(!ToolCodec::validate("novaterm_wait_text", invalid).isEmpty());
        auto start = argumentsFor("novaterm_interact_terminal");
        QVERIFY(ToolCodec::validate("novaterm_start_task", start).isEmpty());
        QVERIFY(!isError(ToolCodec::decodeCall("novaterm_start_task", start)));
        start.insert("timeoutMs", 3600001);
        QVERIFY(!ToolCodec::validate("novaterm_start_task", start).isEmpty());
        auto query = identity(); query.insert("operationId", OperationUuid);
        QVERIFY(ToolCodec::validate("novaterm_query_task", query).isEmpty());
        query.insert("taskId", "task-1");
        QVERIFY(!ToolCodec::validate("novaterm_query_task", query).isEmpty());
        query.remove("operationId");
        QVERIFY(!isError(ToolCodec::decodeCall("novaterm_query_task", query)));
        query.insert("operationId", OperationUuid);
        QVERIFY(!isError(ToolCodec::decodeCall("novaterm_stop_task", query)));
        static_assert(std::variant_size_v<ToolRequest> == 5);
    }
    void clearToolHasIndependentContract()
    {
        auto args = identity();
        args.insert("operationId", OperationUuid);
        args.insert("scope", "scrollback");
        args.insert("expectedGeneration", "1");
        args.insert("clearToken", "signed-clear-baseline");
        QVERIFY(ToolCodec::isKnownTool("novaterm_clear_terminal"));
        QVERIFY(ToolCodec::validate("novaterm_clear_terminal", args).isEmpty());
        QVERIFY(!isError(ToolCodec::decodeCall("novaterm_clear_terminal", args)));
        for (const auto& name : QStringList{"operationId", "scope", "expectedGeneration", "clearToken"}) {
            auto missing = args; missing.remove(name);
            QVERIFY(!ToolCodec::validate("novaterm_clear_terminal", missing).isEmpty());
        }
        for (const auto& generation : QStringList{"-1", "01", "18446744073709551616", ""}) {
            auto invalid = args; invalid.insert("expectedGeneration", generation);
            QVERIFY(!ToolCodec::validate("novaterm_clear_terminal", invalid).isEmpty());
        }
        auto invalid = args; invalid.insert("scope", "screen");
        QVERIFY(!ToolCodec::validate("novaterm_clear_terminal", invalid).isEmpty());
        invalid = args; invalid.insert("path", "C:/private");
        QVERIFY(!ToolCodec::validate("novaterm_clear_terminal", invalid).isEmpty());
        static_assert(std::variant_size_v<ToolRequest> == 5);
        QFile fixture(QStringLiteral(NOVATERM_SOURCE_DIR "/tests/mcp/fixtures/clear-terminal-2026-10-25.json"));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        QCOMPARE(ToolCodec::catalog()[8].toObject(), QJsonDocument::fromJson(fixture.readAll()).object());
    }
    void logReadRequiresExplicitSourceAndStart();
    void catalogMatchesCompatibilityFixture();
    void catalogExposesCurrentToolsInOrder();
    void everyToolDecodesToItsOwnRequest();
    void rejectedArgumentsKeepCompatibleMessages();
    void optionalArgumentsPreservePresence();
    void remainingRequestDefaultsAreStable();
    void everyToolRejectsUnknownFieldsAndInvalidEnums();
    void utf8LimitsCountEncodedBytes();
    void actionsDecodeWithoutFieldOverlap();
    void failureEncodingKeepsEnvelope();
    void errorCodeMappingRoundTrips();
    void strongIdentifiersRejectLooseText();
    void projectionSizerMatchesWireEncoding();
};

void ContractsTests::logReadRequiresExplicitSourceAndStart()
{
    auto args = identity();
    args.insert("source", "log");
    args.insert("start", "oldest");
    QVERIFY2(ToolCodec::validate("novaterm_read_context", args).isEmpty(), "B1 log selection must be accepted");
    args.remove("start");
    QVERIFY(!ToolCodec::validate("novaterm_read_context", args).isEmpty());
    args.insert("sinceToken", "signed-log-position");
    QVERIFY(ToolCodec::validate("novaterm_read_context", args).isEmpty());
    args.insert("start", "tail");
    QVERIFY(!ToolCodec::validate("novaterm_read_context", args).isEmpty());
    args.remove("start");
    for (const auto& field : QStringList{"expandToken", "waitFor", "includeViewport", "waitMs"}) {
        auto invalid = args;
        invalid.insert(field, field == "includeViewport" ? QJsonValue(true)
            : field == "waitMs" ? QJsonValue(1) : QJsonValue("x"));
        QVERIFY(!ToolCodec::validate("novaterm_read_context", invalid).isEmpty());
    }
    args = identity(); args.insert("start", "tail");
    QVERIFY(!ToolCodec::validate("novaterm_read_context", args).isEmpty());
}

void ContractsTests::catalogMatchesCompatibilityFixture()
{
    QFile fixture(QStringLiteral(NOVATERM_SOURCE_DIR)
        + QStringLiteral("/tests/mcp/fixtures/tool-catalog-2026-10-18.json"));
    QVERIFY2(fixture.open(QIODevice::ReadOnly), qPrintable(fixture.errorString()));
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(fixture.readAll(), &error);
    QCOMPARE(error.error, QJsonParseError::NoError);
    QVERIFY(document.isArray());
    // 对象比较，不是文本比较：键顺序与空白不参与语义，字段/枚举/默认值参与。
    const auto legacy = document.array();
    QVERIFY(ToolCodec::catalog().size() >= legacy.size());
    for (qsizetype i = 0; i < legacy.size(); ++i) {
        const auto previousTool = legacy[i].toObject();
        const auto name = previousTool.value("name").toString();
        if (retiredTool(name)) continue;
        const auto current = toolByName(name);
        if (name != "novaterm_read_context") {
            QCOMPARE(current, retainedSchema(previousTool).toObject()); continue;
        }
        QFile logFixture(QStringLiteral(NOVATERM_SOURCE_DIR "/tests/mcp/fixtures/read-context-clear-2026-10-25.json"));
        QVERIFY(logFixture.open(QIODevice::ReadOnly));
        QCOMPARE(current, QJsonDocument::fromJson(logFixture.readAll()).object());
        QFile b1Fixture(QStringLiteral(NOVATERM_SOURCE_DIR "/tests/mcp/fixtures/read-context-log-2026-10-25.json"));
        QVERIFY(b1Fixture.open(QIODevice::ReadOnly));
        const auto b1 = QJsonDocument::fromJson(b1Fixture.readAll()).object();
        auto b2Log = current.value("outputSchema").toObject().value("properties").toObject().value("data").toObject().value("anyOf").toArray().last().toObject();
        auto b2Props = b2Log.value("properties").toObject(); b2Props.remove("clearToken"); b2Log.insert("properties", b2Props);
        auto b2Required = b2Log.value("required").toArray();
        for (qsizetype j = 0; j < b2Required.size(); ++j) if (b2Required[j] == "clearToken") { b2Required.removeAt(j); break; }
        b2Log.insert("required", b2Required);
        QCOMPARE(b2Log, b1.value("outputSchema").toObject().value("properties").toObject().value("data").toObject().value("anyOf").toArray().last().toObject());
        const auto previous = legacy[i].toObject();
        QCOMPARE(current.value("annotations"), previous.value("annotations"));
        const auto oldInput = previous.value("inputSchema").toObject();
        const auto newInput = current.value("inputSchema").toObject();
        QCOMPARE(newInput.value("required"), oldInput.value("required"));
        const auto props = oldInput.value("properties").toObject();
        for (auto it = props.begin(); it != props.end(); ++it)
            QCOMPARE(newInput.value("properties").toObject().value(it.key()), it.value());
        QCOMPARE(current.value("outputSchema").toObject().value("properties").toObject().value("data")
            .toObject().value("anyOf").toArray().first(), previous.value("outputSchema").toObject().value("properties").toObject().value("data"));
    }
}

void ContractsTests::catalogExposesCurrentToolsInOrder()
{
    const auto registry = NovaTerm::Capabilities::CapabilityRegistry::implementedCapabilities();
    QCOMPARE(registry.definitions().size(), size_t(ToolCodec::catalog().size()));
    for (const auto& tool : ToolCodec::catalog()) {
        const auto name = tool.toObject().value("name").toString();
        const auto definition = registry.findByMcpTool(name);
        QVERIFY(definition);
        QVERIFY(definition->id.value() != name);
    }
    QStringList actual;
    for (qsizetype i = 0; i < toolNames().size(); ++i)
        actual.append(ToolCodec::catalog()[i].toObject().value(QStringLiteral("name")).toString());
    QCOMPARE(actual, toolNames());
    for (const auto& name : toolNames())
        QVERIFY2(ToolCodec::isKnownTool(name), qPrintable(name));
    QVERIFY(!ToolCodec::isKnownTool(QStringLiteral("novaterm_unknown")));
}

void ContractsTests::everyToolDecodesToItsOwnRequest()
{
    for (const auto& name : toolNames()) {
        const auto arguments = argumentsFor(name);
        QVERIFY2(ToolCodec::validate(name, arguments).isEmpty(),
            qPrintable(ToolCodec::validate(name, arguments)));
        const auto decoded = ToolCodec::decode(name, arguments);
        QVERIFY2(!isError(decoded), qPrintable(name));
        const auto& request = std::get<ToolRequest>(decoded);
        QCOMPARE(ToolCodec::toolName(request), name);
        const auto reference = sessionReference(request);
        QVERIFY(!reference || (reference->session.str() == SessionUuid
            && reference->epoch.str() == QStringLiteral("epoch-1")));
    }
}

void ContractsTests::rejectedArgumentsKeepCompatibleMessages()
{
    const auto sessionArguments = identity();
    QCOMPARE(ToolCodec::validate(QStringLiteral("novaterm_missing_tool"), {}),
        QStringLiteral("Unknown tool."));

    auto missing = sessionArguments;
    missing.remove(QStringLiteral("epoch"));
    QCOMPARE(ToolCodec::validate(QStringLiteral("novaterm_read_context"), missing),
        QStringLiteral("Missing required argument: epoch"));
    const auto missingDecoded =
        ToolCodec::decode(QStringLiteral("novaterm_read_context"), missing);
    QVERIFY(isError(missingDecoded));
    QCOMPARE(errorIfAny(missingDecoded)->code, ErrorCode::InvalidArgument);
    QCOMPARE(errorIfAny(missingDecoded)->message,
        QStringLiteral("Missing required argument: epoch"));

    auto unknown = sessionArguments;
    unknown.insert(QStringLiteral("extra"), true);
    QCOMPARE(ToolCodec::validate(QStringLiteral("novaterm_read_context"), unknown),
        QStringLiteral("Unknown argument: extra"));

    auto search = argumentsFor(QStringLiteral("novaterm_search_context"));
    search.insert(QStringLiteral("query"), QStringLiteral("line\nbreak"));
    QCOMPARE(ToolCodec::validate(QStringLiteral("novaterm_search_context"), search),
        QStringLiteral("Invalid argument: query"));

    auto expand = sessionArguments;
    expand.insert(QStringLiteral("expandToken"), QStringLiteral("token"));
    expand.insert(QStringLiteral("sinceToken"), QStringLiteral("cursor"));
    QCOMPARE(ToolCodec::validate(QStringLiteral("novaterm_read_context"), expand),
        QStringLiteral("expandToken and sinceToken are mutually exclusive."));

    auto mixed = argumentsFor(QStringLiteral("novaterm_interact_terminal"));
    mixed.insert(QStringLiteral("actions"), QJsonArray{
        QJsonObject{{QStringLiteral("type"), QStringLiteral("command")},
            {QStringLiteral("command"), QStringLiteral("uptime")}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
            {QStringLiteral("text"), QStringLiteral("x")}}});
    QCOMPARE(ToolCodec::validate(QStringLiteral("novaterm_interact_terminal"), mixed),
        QStringLiteral("command and release must be the only action."));

    auto elevation = argumentsFor(QStringLiteral("novaterm_request_elevation"));
    elevation.insert(QStringLiteral("command"), QStringLiteral("id"));
    QCOMPARE(ToolCodec::validate(QStringLiteral("novaterm_request_elevation"), elevation),
        QStringLiteral("command is required only in command mode."));
}

void ContractsTests::optionalArgumentsPreservePresence()
{
    const auto name = QStringLiteral("novaterm_read_context");
    const auto minimal = ToolCodec::decode(name, identity());
    QVERIFY(!isError(minimal));
    const auto& plain = std::get<ReadContextRequest>(std::get<ToolRequest>(minimal));
    QVERIFY(!plain.projection.has_value());
    QVERIFY(!plain.wait.has_value());
    QVERIFY(!plain.waitFor.has_value());
    QVERIFY(!plain.includeViewport.has_value());
    QVERIFY(!plain.maxBytes.has_value());
    QVERIFY(!plain.expandToken.has_value());

    auto arguments = identity();
    arguments.insert(QStringLiteral("responseMode"), QStringLiteral("compact"));
    arguments.insert(QStringLiteral("waitMs"), 0);
    arguments.insert(QStringLiteral("waitFor"), QStringLiteral("change"));
    arguments.insert(QStringLiteral("includeViewport"), false);
    arguments.insert(QStringLiteral("maxBytes"), 4096);
    const auto decoded = ToolCodec::decode(name, arguments);
    QVERIFY(!isError(decoded));
    const auto& request = std::get<ReadContextRequest>(std::get<ToolRequest>(decoded));
    QCOMPARE(request.projection.value_or(ProjectionMode::Full), ProjectionMode::Compact);
    QCOMPARE(request.wait->count(), 0);
    QCOMPARE(request.waitFor.value_or(WaitCondition::Attention), WaitCondition::Change);
    QCOMPARE(request.includeViewport.value_or(true), false);
    QCOMPARE(request.maxBytes.value_or(65536), 4096);
}

void ContractsTests::remainingRequestDefaultsAreStable()
{
    const auto list = ToolCodec::decode(QStringLiteral("novaterm_list_sessions"), {});
    QVERIFY(!isError(list));
    const auto& listed = std::get<ListSessionsRequest>(std::get<ToolRequest>(list));
    QCOMPARE(listed.limit, 50);
    QVERIFY(!listed.cursor);

    const auto search = ToolCodec::decode(QStringLiteral("novaterm_search_context"),
        argumentsFor(QStringLiteral("novaterm_search_context")));
    QVERIFY(!isError(search));
    const auto& searched = std::get<SearchContextRequest>(std::get<ToolRequest>(search));
    QCOMPARE(searched.scope, SearchScope::Both);
    QVERIFY(searched.caseSensitive);
    QCOMPARE(searched.maxMatches, 20);

    const auto input = ToolCodec::decode(QStringLiteral("novaterm_interact_terminal"),
        argumentsFor(QStringLiteral("novaterm_interact_terminal")));
    QVERIFY(!isError(input));
    const auto& interacted = std::get<InteractTerminalRequest>(std::get<ToolRequest>(input));
    QVERIFY(!interacted.wait && !interacted.waitFor && !interacted.projection);
    QVERIFY(!interacted.replyToExecution && !interacted.requireVerifiedReady);
    const auto elevation = ToolCodec::decode(QStringLiteral("novaterm_request_elevation"),
        argumentsFor(QStringLiteral("novaterm_request_elevation")));
    QVERIFY(!isError(elevation));
    const auto& elevated = std::get<RequestElevationRequest>(std::get<ToolRequest>(elevation));
    QVERIFY(!elevated.wait && !elevated.waitFor && !elevated.projection && !elevated.command);
}

void ContractsTests::everyToolRejectsUnknownFieldsAndInvalidEnums()
{
    for (const auto& tool : toolNames()) {
        auto args = argumentsFor(tool);
        args.insert(QStringLiteral("extra"), true);
        const auto result = ToolCodec::decode(tool, args);
        QVERIFY2(isError(result), qPrintable(tool));
        QCOMPARE(errorIfAny(result)->code, ErrorCode::InvalidArgument);
        QCOMPARE(errorIfAny(result)->message, QStringLiteral("Unknown argument: extra"));
    }
    const QList<QPair<QString, QStringList>> fields{
        {QStringLiteral("novaterm_read_context"), {QStringLiteral("responseMode"), QStringLiteral("waitFor")}},
        {QStringLiteral("novaterm_search_context"), {QStringLiteral("scope")}},
        {QStringLiteral("novaterm_interact_terminal"), {QStringLiteral("responseMode"), QStringLiteral("waitFor")}},
        {QStringLiteral("novaterm_request_elevation"), {QStringLiteral("responseMode"), QStringLiteral("waitFor"),
            QStringLiteral("method"), QStringLiteral("mode")}},
    };
    for (const auto& [tool, names] : fields) {
        for (const auto& field : names) {
            auto args = argumentsFor(tool);
            args.insert(field, QStringLiteral("unsupported"));
            const auto result = ToolCodec::decode(tool, args);
            QVERIFY2(isError(result), qPrintable(tool + QLatin1Char('/') + field));
            QCOMPARE(errorIfAny(result)->message, QStringLiteral("Invalid argument: ") + field);
        }
    }
}

void ContractsTests::utf8LimitsCountEncodedBytes()
{
    const QString scalar = QString::fromUtf8("😀");
    for (const auto& [tool, field, scalars] : {
             std::tuple{QStringLiteral("novaterm_search_context"), QStringLiteral("query"), 256}}) {
        auto args = argumentsFor(tool);
        const auto boundary = scalar.repeated(scalars);
        QCOMPARE(boundary.toUtf8().size(), scalars * 4);
        args.insert(field, boundary);
        QVERIFY(!isError(ToolCodec::decode(tool, args)));
        args.insert(field, boundary + QLatin1Char('x'));
        const auto oversized = ToolCodec::decode(tool, args);
        QVERIFY(isError(oversized));
        QCOMPARE(errorIfAny(oversized)->message, QStringLiteral("Invalid argument: ") + field);
    }
}

void ContractsTests::actionsDecodeWithoutFieldOverlap()
{
    auto arguments = argumentsFor(QStringLiteral("novaterm_interact_terminal"));
    arguments.insert(QStringLiteral("actions"), QJsonArray{
        QJsonObject{{QStringLiteral("type"), QStringLiteral("key")},
            {QStringLiteral("key"), QStringLiteral("c")},
            {QStringLiteral("modifiers"), QJsonArray{QStringLiteral("Ctrl")}}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
            {QStringLiteral("text"), QStringLiteral("ls")}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("line_ending")}}});
    const auto decoded = ToolCodec::decode(QStringLiteral("novaterm_interact_terminal"), arguments);
    QVERIFY(!isError(decoded));
    const auto& request = std::get<InteractTerminalRequest>(std::get<ToolRequest>(decoded));
    QCOMPARE(request.actions.size(), std::size_t(3));
    const auto* key = std::get_if<KeyAction>(&request.actions[0]);
    QVERIFY(key);
    QCOMPARE(key->key, QStringLiteral("c"));
    QCOMPARE(key->modifiers.size(), std::size_t(1));
    QCOMPARE(key->modifiers[0], KeyModifier::Ctrl);
    QVERIFY(std::holds_alternative<TextAction>(request.actions[1]));
    QVERIFY(std::holds_alternative<LineEndingAction>(request.actions[2]));
}

void ContractsTests::failureEncodingKeepsEnvelope()
{
    Error busy{ErrorCode::Busy, QStringLiteral("queue full"),
        RetryHint{true, std::chrono::milliseconds{250}}};
    const auto encoded = ToolCodec::encodeFailure(busy);
    const QJsonObject expected{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("ok"), false},
        {QStringLiteral("error"), QJsonObject{
            {QStringLiteral("code"), QStringLiteral("BUSY")},
            {QStringLiteral("message"), QStringLiteral("queue full")},
            {QStringLiteral("retryable"), true},
            {QStringLiteral("retryAfterMs"), 250}}}};
    QCOMPARE(encoded, expected);

    // 历史 `error(code)` 形态：消息缺省使用错误码名称，且不出现可重试字段值。
    const auto unnamed = ToolCodec::encodeFailure(Error{ErrorCode::SessionNotAvailable});
    QCOMPARE(unnamed.value(QStringLiteral("error")).toObject()
                 .value(QStringLiteral("message")).toString(),
        QStringLiteral("SESSION_NOT_AVAILABLE"));
    QCOMPARE(unnamed.value(QStringLiteral("error")).toObject()
                 .value(QStringLiteral("retryable")).toBool(),
        false);
    QVERIFY(!unnamed.value(QStringLiteral("error")).toObject()
        .contains(QStringLiteral("retryAfterMs")));
}

void ContractsTests::errorCodeMappingRoundTrips()
{
    const QList<ErrorCode> codes{ErrorCode::InvalidArgument, ErrorCode::Busy,
        ErrorCode::DeadlineExceeded, ErrorCode::CommandConfirmationStale,
        ErrorCode::ScriptPublicationOutcomeUnknown, ErrorCode::SessionCommandNotReady};
    for (const auto code : codes) {
        const auto name = QString::fromLatin1(errorCodeName(code));
        QVERIFY(!name.isEmpty());
        QCOMPARE(errorCodeFromName(name).value_or(ErrorCode::InvalidArgument), code);
    }
    QVERIFY(!errorCodeFromName(QStringLiteral("NOT_A_REAL_CODE")).has_value());
}

void ContractsTests::strongIdentifiersRejectLooseText()
{
    QVERIFY(isCanonicalUuid(SessionUuid));
    // 与重构前 QUuid 往返判据一致：大小写规范化后相等即接受。
    QVERIFY(isCanonicalUuid(SessionUuid.toUpper()));
    QVERIFY(!isCanonicalUuid(QStringLiteral("{") + SessionUuid + QStringLiteral("}")));
    QVERIFY(!parseUuidId<SessionId>(QStringLiteral("not-a-uuid")).has_value());
    QVERIFY(!parseUuidId<SessionId>(QString{}).has_value());
    QCOMPARE(parseUuidId<SessionId>(SessionUuid)->str(), SessionUuid);
    QVERIFY(!parseBoundedId<Epoch>(QString{}, 64).has_value());
    QVERIFY(!parseBoundedId<Epoch>(QString(65, QLatin1Char('x')), 64).has_value());
    QVERIFY(parseBoundedId<Epoch>(QString(64, QLatin1Char('x')), 64).has_value());
    // 不同类型的 ID 不能互相赋值；相等比较只在同类型内成立。
    const SessionId session{SessionUuid};
    const Epoch epoch{SessionUuid};
    QCOMPARE(session.str(), epoch.str());
}

void ContractsTests::projectionSizerMatchesWireEncoding()
{
    const QJsonObject payload{{QStringLiteral("text"), QString(4096, QLatin1Char('x'))},
        {QStringLiteral("rows"), 24}};
    const auto expected = QJsonDocument(
        toolResult(success(payload))).toJson(QJsonDocument::Compact).size();
    QCOMPARE(ProtocolProjectionSizer::encodedToolResultBytes(payload), expected);
    QVERIFY(expected > 4096);
}



int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (application.arguments().size() == 3 && application.arguments()[1] == "--write-file-overwrite-fixture") {
        const auto tools = ToolCodec::catalog();
        if (tools.size() != 20) return 1;
        QFile file(application.arguments()[2]);
        if (!file.open(QIODevice::WriteOnly)) return 1;
        const auto bytes = QJsonDocument(QJsonArray{toolByName("novaterm_upload_file"), toolByName("novaterm_download_file")}).toJson();
        return file.write(bytes) == bytes.size() ? 0 : 1;
    }
    if (application.arguments().size() == 3 && application.arguments()[1] == "--write-file-download-fixture") {
        const auto tools = ToolCodec::catalog();
        if (tools.size() != 20) return 1;
        QFile file(application.arguments()[2]);
        if (!file.open(QIODevice::WriteOnly)) return 1;
        const auto bytes = QJsonDocument(toolByName("novaterm_download_file")).toJson();
        return file.write(bytes) == bytes.size() ? 0 : 1;
    }
    if (application.arguments().size() == 3 && application.arguments()[1] == "--write-serial-transfer-fixture") {
        const auto tools = ToolCodec::catalog();
        if (tools.size() != 20) return 1;
        QFile file(application.arguments()[2]);
        if (!file.open(QIODevice::WriteOnly)) return 1;
        const auto bytes = QJsonDocument(QJsonArray{toolByName("novaterm_upload_serial_file"), toolByName("novaterm_query_serial_transfer"), toolByName("novaterm_cancel_serial_transfer")}).toJson();
        return file.write(bytes) == bytes.size() ? 0 : 1;
    }
    if (application.arguments().size() == 3 && application.arguments()[1] == "--write-file-transfer-fixture") {
        const QJsonArray appended{toolByName("novaterm_upload_file"), toolByName("novaterm_query_transfer"), toolByName("novaterm_cancel_transfer")};
        QFile file(application.arguments()[2]);
        if (!file.open(QIODevice::WriteOnly)) return 1;
        const auto bytes = QJsonDocument(appended).toJson();
        return file.write(bytes) == bytes.size() ? 0 : 1;
    }
    if (application.arguments().size() == 3 && application.arguments()[1] == "--write-workflow-fixture") {
        QJsonArray workflow;
        for (const auto* name : {"novaterm_wait_text", "novaterm_start_task", "novaterm_query_task", "novaterm_stop_task"})
            workflow.append(toolByName(QString::fromLatin1(name)));
        QFile file(application.arguments()[2]);
        if (!file.open(QIODevice::WriteOnly)) return 1;
        const auto bytes = QJsonDocument(workflow).toJson();
        return file.write(bytes) == bytes.size() ? 0 : 1;
    }
    ContractsTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "ContractsTests.moc"
