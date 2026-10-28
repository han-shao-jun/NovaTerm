/** @file ReadUseCasesTests.cpp @brief 只链接纯应用目标的读取边界验收。 */
#include "mcp/tasks/ReadUseCases.h"
#include "mcp/tasks/LogReadUseCase.h"

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
    bool clearDuringRead{false};
    mutable int pageCalls{0};
    mutable int readCalls{0};
    mutable ContextQuery lastRead;
    std::optional<Error> readError;
    ContextSummary summary;

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
        auto result = summary;
        if (clearDuringRead) ++result.cacheGeneration;
        return result;
    }
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
    void contextRejectsOldGenerationAndConcurrentClear()
    {
        FakeSessions port;
        auto query = contextQuery(); query.cacheGeneration = 7;
        port.summary.cacheGeneration = 7;
        auto first = ReadUseCases{port}.readContext(client, query, std::nullopt);
        QVERIFY(!isError(first));
        const auto cursor = std::get<ContextPage>(first).next;
        QVERIFY(cursor);
        query.cacheGeneration = 8;
        const auto stale = ReadUseCases{port}.readContext(client, query, cursor);
        QVERIFY(isError(stale));
        QCOMPARE(errorIfAny(stale)->code, ErrorCode::CursorStale);
        QCOMPARE(port.readCalls, 1);
        query.cacheGeneration = 7; port.clearDuringRead = true;
        const auto raced = ReadUseCases{port}.readContext(client, query, std::nullopt);
        QVERIFY(isError(raced));
        QCOMPARE(errorIfAny(raced)->code, ErrorCode::CursorStale);
    }
    void logReadRechecksGrantAndBoundsWithoutMutableState()
    {
        class Port final : public LogReadPort {
        public:
            mutable int calls{0}; mutable quint64 grant{1}; mutable bool allowed{true};
            bool revoke{false}, changeVersion{false};
            bool authorized(const ClientId&, const SessionReference&) const override { return allowed; }
            ClientAccessVersion version(const ClientId&) const override { return ClientAccessVersion{grant}; }
            Result<NovaTerm::LogPage> read(const SessionReference&, const NovaTerm::LogRequest&) const override {
                ++calls; if (revoke) allowed = false; if (changeVersion) ++grant;
                return NovaTerm::LogPage{};
            }
        } port;
        const SessionReference target{SessionId{"s"}, Epoch{"e"}};
        NovaTerm::LogRequest query;
        QVERIFY(!isError(readLog(port, client, target, query, ClientAccessVersion{1})));
        query.maxBytes = 0; QVERIFY(isError(readLog(port, client, target, query, {}))); QCOMPARE(port.calls, 1);
        query.maxBytes = 8;
        QVERIFY(isError(readLog(port, client, target, query, ClientAccessVersion{2}))); QCOMPARE(port.calls, 1);
        port.changeVersion = true;
        QCOMPARE(errorIfAny(readLog(port, client, target, query, {}))->code, ErrorCode::CursorStale);
        port.changeVersion = false; port.revoke = true;
        QCOMPARE(errorIfAny(readLog(port, client, target, query, {}))->code, ErrorCode::SessionNotAvailable);
    }
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


};

QTEST_GUILESS_MAIN(ReadUseCasesTests)
#include "ReadUseCasesTests.moc"
