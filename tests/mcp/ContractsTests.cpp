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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <cstddef>
#include <type_traits>
#include <tuple>

using namespace NovaTerm::Mcp;

namespace {
const QString SessionUuid = QStringLiteral("11111111-1111-4111-8111-111111111111");
const QString OperationUuid = QStringLiteral("22222222-2222-4222-8222-222222222222");

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
        QStringLiteral("novaterm_list_commands"),
        QStringLiteral("novaterm_execute_command"),
        QStringLiteral("novaterm_run_command"),
        QStringLiteral("novaterm_run_script"),
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
        static_assert(std::variant_size_v<ToolRequest> == 9);
        QFile fixture(QStringLiteral(NOVATERM_SOURCE_DIR "/tests/mcp/fixtures/clear-terminal-2026-10-25.json"));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        QCOMPARE(ToolCodec::catalog().last().toObject(), QJsonDocument::fromJson(fixture.readAll()).object());
    }
    void logReadRequiresExplicitSourceAndStart();
    void confirmationClaimsKeepWireFormatAndRejectTampering();
    void catalogMatchesCompatibilityFixture();
    void catalogExposesNineToolsInOrder();
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
        const auto current = ToolCodec::catalog()[i].toObject();
        if (legacy[i].toObject().value("name") != "novaterm_read_context") {
            QCOMPARE(current, legacy[i].toObject()); continue;
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

void ContractsTests::catalogExposesNineToolsInOrder()
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

    for (const auto& tool : {QStringLiteral("novaterm_run_command"), QStringLiteral("novaterm_run_script")}) {
        const auto result = ToolCodec::decode(tool, argumentsFor(tool));
        QVERIFY(!isError(result));
        QCOMPARE(std::visit([](const auto& request) -> qint64 {
            using Type = std::decay_t<decltype(request)>;
            if constexpr (std::is_same_v<Type, RunCommandRequest> || std::is_same_v<Type, RunScriptRequest>)
                return request.timeout.count();
            else return -1;
        }, std::get<ToolRequest>(result)), 5000);
    }
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
             std::tuple{QStringLiteral("novaterm_run_command"), QStringLiteral("command"), 4096},
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

void ContractsTests::confirmationClaimsKeepWireFormatAndRejectTampering()
{
    ConfirmationBinding binding;
    binding.connection = PeerId{QStringLiteral("peer")};
    binding.client = ClientId{QStringLiteral("client")};
    binding.session = SessionId{SessionUuid};
    binding.epoch = Epoch{QStringLiteral("epoch")};
    binding.targetFingerprint = QStringLiteral("target");
    binding.policyVersion = QStringLiteral("policy");
    binding.profileVersion = QStringLiteral("profile");
    binding.grantVersion = ClientAccessVersion{7};
    binding.promptGeneration = PromptRevision{8};
    binding.payloadHash = QByteArray(32, 'h');
    const QByteArray key{"secret"};
    for (auto tool : {ConfirmationTool::Command, ConfirmationTool::Script}) {
        for (auto user : {std::optional<InputRevision>{}, std::optional<InputRevision>{InputRevision{0}}}) {
            binding.tool = tool;
            binding.userInputGeneration = user;
            const ConfirmationTransaction transaction{binding, QStringLiteral("nonce"), 1000};
            const auto claims = ToolCodec::encodeConfirmation(transaction);
            QCOMPARE(claims.value("kind").toString(), QStringLiteral("interactive-confirmation"));
            QCOMPARE(claims.contains("tool"), tool == ConfirmationTool::Script);
            QCOMPARE(claims.contains("payloadHash"), tool == ConfirmationTool::Script);
            QCOMPARE(claims.contains("commandHash"), tool == ConfirmationTool::Command);
            QCOMPARE(claims.contains("decision"), tool == ConfirmationTool::Command);
            QCOMPARE(claims.contains("userInputGeneration"), user.has_value());
            QCOMPARE(claims.value("grantVersion").toString(), QStringLiteral("7"));
            const auto token = ToolCodec::signConfirmation(transaction, key);
            QCOMPARE(token, signToken(claims, key));
            const auto verified = ToolCodec::verifyConfirmation(token, key);
            QVERIFY(verified);
            QCOMPARE(ToolCodec::encodeConfirmation(*verified), claims);
            QVERIFY(!ToolCodec::verifyConfirmation(token, QByteArray{"other"}));
            auto tampered = token;
            tampered[0] = tampered[0] == QLatin1Char('a') ? QLatin1Char('b') : QLatin1Char('a');
            QVERIFY(!ToolCodec::verifyConfirmation(tampered, key));

            for (const auto* field : {"client", "connection", "session", "epoch", "target",
                     "policyVersion", "profileVersion", "grantVersion", "promptGeneration",
                     "nonce", "expires"}) {
                auto missing = claims;
                missing.remove(QLatin1String(field));
                QVERIFY(!ToolCodec::decodeConfirmation(missing));
            }
            for (const auto& invalid : {QJsonValue{-1}, QJsonValue{1.5}, QJsonValue{"1000"}}) {
                auto malformed = claims;
                malformed.insert("expires", invalid);
                QVERIFY(!ToolCodec::decodeConfirmation(malformed));
            }
            auto malformed = claims;
            malformed.insert("grantVersion", QStringLiteral("-1"));
            QVERIFY(!ToolCodec::decodeConfirmation(malformed));
            malformed = claims;
            malformed.insert("userInputGeneration", QJsonValue::Null);
            QVERIFY(!ToolCodec::decodeConfirmation(malformed));
            malformed = claims;
            malformed.insert(tool == ConfirmationTool::Script ? "payloadHash" : "commandHash", "invalid");
            QVERIFY(!ToolCodec::decodeConfirmation(malformed));
        }
    }
}

QTEST_MAIN(ContractsTests)
#include "ContractsTests.moc"
