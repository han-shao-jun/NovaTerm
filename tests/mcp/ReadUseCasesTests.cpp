/** @file ReadUseCasesTests.cpp @brief 只链接纯应用目标的读取边界验收。 */
#include "mcp/tasks/ReadUseCases.h"

#include <QTest>

using namespace NovaTerm::Mcp;

namespace {
const ClientId client{QStringLiteral("client")};
ReadSession session(const char* id)
{
    ReadSession result;
    result.reference = {SessionId{QString::fromLatin1(id)}, Epoch{QStringLiteral("epoch")}};
    result.running = true;
    return result;
}

class FakeSessions final : public ReadSessionPort
{
public:
    mutable quint64 revision{4};
    mutable quint64 grant{8};
    QList<ReadSession> rows{session("a"), session("b"), session("c")};
    bool changeDirectoryDuringPage{false};
    bool revokeDuringRead{false};
    mutable int pageCalls{0};
    mutable int readCalls{0};
    mutable ContextQuery lastRead;
    std::optional<Error> readError;
    ContextSummary summary;
    DiagnosticCatalog catalog;

    quint64 directoryRevision() const override { return revision; }
    ClientAccessVersion accessVersion(const ClientId&) const override
    { return ClientAccessVersion{grant}; }
    QList<ReadSession> pageAfter(const ClientId&, const SessionId& after,
        int limit, bool) const override
    {
        ++pageCalls;
        if (changeDirectoryDuringPage) ++revision;
        QList<ReadSession> page;
        for (const auto& row : rows) {
            if (row.visible && row.reference.session.str() > after.str()) page.append(row);
            if (page.size() == limit) break;
        }
        return page;
    }
    Result<ContextSummary> readContext(const ContextQuery& query) const override
    {
        ++readCalls;
        lastRead = query;
        if (revokeDuringRead) ++grant;
        if (readError) return *readError;
        return summary;
    }
    DiagnosticCatalog commandCatalog(const ClientId&, const SessionReference&) const override
    { return catalog; }
};

ContextQuery contextQuery()
{
    ContextQuery query;
    query.session = session("a").reference;
    query.capture = CaptureId{QStringLiteral("capture")};
    query.owner = QStringLiteral("owner");
    return query;
}
} // namespace

class ReadUseCasesTests : public QObject
{
    Q_OBJECT
private slots:
    void paginationIsBoundedAndResumesAfterLastVisibleSession()
    {
        FakeSessions port;
        port.rows.insert(1, session("ab"));
        port.rows[1].visible = false;
        ReadUseCases reads{port};
        const auto first = reads.listSessions(client, 2, std::nullopt, false);
        QVERIFY(!isError(first));
        const auto& page = std::get<SessionPage>(first);
        QCOMPARE(page.sessions.size(), 2);
        QCOMPARE(page.sessions[0].reference.session.str(), QStringLiteral("a"));
        QCOMPARE(page.sessions[1].reference.session.str(), QStringLiteral("b"));
        QVERIFY(page.next);
        QCOMPARE(page.next->after.str(), QStringLiteral("b"));
        QCOMPARE(page.next->directoryRevision, port.revision);
        QCOMPARE(page.next->accessVersion.value(), port.grant);
        const auto second = reads.listSessions(client, 2, page.next, false);
        QVERIFY(!isError(second));
        const auto& tail = std::get<SessionPage>(second);
        QCOMPARE(tail.sessions.size(), 1);
        QCOMPARE(tail.sessions[0].reference.session.str(), QStringLiteral("c"));
        QVERIFY(!tail.next);
    }

    void staleDirectoryOrGrantNeverReadsAnotherPage()
    {
        FakeSessions port;
        ReadUseCases reads{port};
        auto cursor = DirectoryCursor{SessionId{QStringLiteral("a")}, port.revision, ClientAccessVersion{port.grant}};
        ++port.revision;
        auto result = reads.listSessions(client, 2, cursor, false);
        QVERIFY(isError(result));
        QCOMPARE(errorIfAny(result)->code, ErrorCode::CursorStale);
        QCOMPARE(port.pageCalls, 0);
        cursor.directoryRevision = port.revision;
        ++port.grant;
        result = reads.listSessions(client, 2, cursor, false);
        QVERIFY(isError(result));
        QCOMPARE(errorIfAny(result)->code, ErrorCode::CursorStale);
        QCOMPARE(port.pageCalls, 0);
        port.changeDirectoryDuringPage = true;
        result = reads.listSessions(client, 2, std::nullopt, false);
        QVERIFY(isError(result));
        QCOMPARE(errorIfAny(result)->code, ErrorCode::CursorStale);
        result = reads.listSessions(client, 0, std::nullopt, false);
        QVERIFY(isError(result));
        QCOMPARE(errorIfAny(result)->code, ErrorCode::InvalidArgument);
    }

    void contextCursorBindsSessionEpochViewportAndGrant_data()
    {
        QTest::addColumn<QString>("field");
        for (const char* field : {"session", "epoch", "viewport", "grant"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }

    void contextCursorBindsSessionEpochViewportAndGrant()
    {
        QFETCH(QString, field);
        FakeSessions port;
        const auto query = contextQuery();
        ContextCursor cursor{query.session, 17, ClientAccessVersion{port.grant}, true};
        if (field == "session") cursor.session.session = SessionId{QStringLiteral("b")};
        if (field == "epoch") cursor.session.epoch = Epoch{QStringLiteral("reconnected")};
        if (field == "viewport") cursor.includeViewport = false;
        if (field == "grant") cursor.accessVersion = ClientAccessVersion{port.grant - 1};
        const auto result = ReadUseCases{port}.readContext(client, query, cursor);
        QVERIFY(isError(result));
        QCOMPARE(errorIfAny(result)->code, ErrorCode::CursorStale);
        QCOMPARE(port.readCalls, 0);
    }

    void contextResumesAndDoesNotIssueCursorAfterTruncationOrRevocation()
    {
        FakeSessions port;
        auto query = contextQuery();
        query.maxBytes = 73;
        query.maxLines = 3;
        port.summary.revision = 23;
        port.summary.capture = query.capture;
        port.summary.recent = {QStringLiteral("output")};
        const ContextCursor cursor{query.session, 17, ClientAccessVersion{port.grant}, true};
        auto result = ReadUseCases{port}.readContext(client, query, cursor);
        QVERIFY(!isError(result));
        const auto& page = std::get<ContextPage>(result);
        QVERIFY(page.next);
        QCOMPARE(page.next->revision, quint64(23));
        QCOMPARE(page.summary.recent, port.summary.recent);
        QCOMPARE(port.lastRead.client.str(), client.str());
        QCOMPARE(port.lastRead.sinceRevision, std::optional<quint64>{17});
        QCOMPARE(port.lastRead.maxBytes, qsizetype(73));
        QCOMPARE(port.lastRead.maxLines, 3);
        port.summary.outputTruncated = true;
        result = ReadUseCases{port}.readContext(client, query, cursor);
        QVERIFY(!isError(result));
        QVERIFY(!std::get<ContextPage>(result).next);
        port.summary.outputTruncated = false;
        port.summary.sourceTruncated = true;
        result = ReadUseCases{port}.readContext(client, query, cursor);
        QVERIFY(!isError(result));
        QVERIFY(!std::get<ContextPage>(result).next);
        port.revokeDuringRead = true;
        result = ReadUseCases{port}.readContext(client, query, cursor);
        QVERIFY(isError(result));
        QCOMPARE(errorIfAny(result)->code, ErrorCode::CursorStale);
    }

    void readErrorsPreserveRetryAdvice()
    {
        FakeSessions port;
        port.readError = Error{ErrorCode::Busy, QStringLiteral("BUSY"), RetryHint{true, std::chrono::milliseconds(50)}};
        auto result = ReadUseCases{port}.readContext(client, contextQuery(), std::nullopt);
        QVERIFY(isError(result));
        QCOMPARE(errorIfAny(result)->code, ErrorCode::Busy);
        QVERIFY(errorIfAny(result)->retry);
        QVERIFY(errorIfAny(result)->retry->retryable);
        QCOMPARE(errorIfAny(result)->retry->after.count(), 50);
        port.readError = Error{ErrorCode::SessionNotAvailable};
        result = ReadUseCases{port}.readContext(client, contextQuery(), std::nullopt);
        QVERIFY(isError(result));
        QCOMPARE(errorIfAny(result)->code, ErrorCode::SessionNotAvailable);
    }

    void commandCatalogFiltersPermissionsAndBindsTickets()
    {
        FakeSessions port;
        port.catalog.policyVersion = QStringLiteral("policy-v1");
        port.catalog.profileVersion = QStringLiteral("profile-v2");
        port.catalog.profileAvailable = port.catalog.executorAvailable = port.catalog.targetVerified = true;
        const DiagnosticId allowed{QStringLiteral("os.uname")};
        port.catalog.allowed.insert(allowed);
        port.catalog.commands = {{allowed, QStringLiteral("System"), QStringLiteral("uname")},
            {DiagnosticId{QStringLiteral("memory")}, QStringLiteral("Memory"), QStringLiteral("free")}};
        auto result = ReadUseCases{port}.listCommands(client, session("a"), 1000);
        QVERIFY(!isError(result));
        const auto& list = std::get<CommandList>(result);
        QVERIFY(!list.disabledReason);
        QCOMPARE(list.commands.size(), 1);
        QCOMPARE(list.commands[0].id.str(), allowed.str());
        QCOMPARE(list.accessVersion.value(), port.grant);
        QCOMPARE(list.expires, qint64(1000 + Budget::ConfirmationTimeout.count()));
        QCOMPARE(list.policyVersion, port.catalog.policyVersion);
        QCOMPARE(list.profileVersion, port.catalog.profileVersion);
        port.catalog.allowed.clear();
        result = ReadUseCases{port}.listCommands(client, session("a"), 1000);
        QVERIFY(!isError(result));
        QCOMPARE(std::get<CommandList>(result).disabledReason,
            std::optional<ErrorCode>{ErrorCode::CommandPermissionRequired});
        QVERIFY(std::get<CommandList>(result).commands.isEmpty());
    }
};

QTEST_GUILESS_MAIN(ReadUseCasesTests)
#include "ReadUseCasesTests.moc"
