#include "mcp/protocol/ToolCodec.h"
#include <QFile>
#include <QJsonDocument>
#include <QTest>
using namespace NovaTerm::Mcp;
class HistoryContractsTests final : public QObject {
    Q_OBJECT
private slots:
    void terminalRequestSetStaysFrozen() {
        static_assert(std::variant_size_v<ToolRequest> == 9, "New domains must not expand terminal application requests");
        QCOMPARE(std::variant_size_v<ToolRequest>, size_t(9));
    }
    void newCatalogMatchesFrozenFixture() {
        QFile fixture(QStringLiteral(NOVATERM_SOURCE_DIR "/tests/mcp/fixtures/history-tool-catalog-2026-10-23.json"));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        const auto expected = QJsonDocument::fromJson(fixture.readAll()).array();
        QJsonArray actual; const auto all = ToolCodec::catalog();
        QCOMPARE(all.size(), 30);
        for (qsizetype i = 9; i < 12; ++i) actual.append(all[i]);
        QCOMPARE(actual, expected);
    }
    void legacyCatalogObjectsUnchanged() {
        QFile fixture(QStringLiteral(NOVATERM_SOURCE_DIR "/tests/mcp/fixtures/tool-catalog-2026-10-18.json"));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        const auto legacy = QJsonDocument::fromJson(fixture.readAll()).array();
        const auto current = ToolCodec::catalog();
        QCOMPARE(current.size(), legacy.size() + 21);
        for (qsizetype i = 0; i < legacy.size(); ++i) {
            if (legacy[i].toObject().value("name") == "novaterm_read_context") {
                QFile logFixture(QStringLiteral(NOVATERM_SOURCE_DIR "/tests/mcp/fixtures/read-context-clear-2026-10-25.json"));
                QVERIFY(logFixture.open(QIODevice::ReadOnly));
                QCOMPARE(current[i].toObject(), QJsonDocument::fromJson(logFixture.readAll()).object());
            } else QCOMPARE(current[i], legacy[i]);
        }
    }
    void historyToolsDecodeWithoutSessionIdentity() {
        const QString capture = "11111111-1111-4111-8111-111111111111";
        const QList<QPair<QString, QJsonObject>> cases{
            {"novaterm_list_captures", {}},
            {"novaterm_read_history", {{"captureId", capture}}},
            {"novaterm_search_history", {{"captureId", capture}, {"query", "error"}}}};
        for (const auto& item : cases) {
            QVERIFY(ToolCodec::isKnownTool(item.first));
            const auto decoded = ToolCodec::decodeCall(item.first, item.second);
            QVERIFY2(!isError(decoded), qPrintable(item.first));
            const auto& request = std::get<ToolCall>(decoded);
            QCOMPARE(ToolCodec::toolName(request), item.first);
            QVERIFY(std::holds_alternative<HistoryToolRequest>(request));
        }
    }
    void historyRejectsPathsMalformedIdsAndInvalidBinaryQuery() {
        const QString capture = "11111111-1111-4111-8111-111111111111";
        const QList<QPair<QString, QJsonObject>> cases{
            {"novaterm_list_captures", {{"path", "C:/private"}}},
            {"novaterm_read_history", {{"captureId", "short-observation-id"}}},
            {"novaterm_read_history", {{"captureId", capture}, {"maxBytes", 65537}}},
            {"novaterm_search_history", {{"captureId", capture}, {"query", "***"}, {"mode", "raw_bytes"}}},
            {"novaterm_search_history", {{"captureId", capture}, {"query", ""}}},
            {"novaterm_search_history", {{"captureId", capture}, {"query", "error"}, {"mode", "regex"}}}};
        for (const auto& item : cases) QVERIFY(isError(ToolCodec::decodeCall(item.first, item.second)));
        QVERIFY(!isError(ToolCodec::decodeCall("novaterm_search_history", {{"captureId", capture}, {"query", "AP8="}, {"mode", "raw_bytes"}})));
    }
};
QTEST_GUILESS_MAIN(HistoryContractsTests)
#include "HistoryContractsTests.moc"
