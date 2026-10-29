#include "mcp/TerminalTextWait.h"
#include "mcp/tasks/TextWaitMatcher.h"
#include "mcp/McpAccess.h"
#include "mcp/McpProtocol.h"
#include "mcp/LogReadAdapter.h"
#include "mcp/ObservationService.h"
#include "session/SessionDirectory.h"
#include "TestGrantSupport.h"
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDir>
#include <QSet>
#include <QTest>
using namespace NovaTerm::Mcp;
namespace {
class OfflineTransport final : public ITransport {
public:
    bool connectToHost() override { online = true; emit connected(); return true; }
    void disconnect() override { online = false; emit disconnected(); }
    void write(const QByteArray& bytes) override { written += bytes; }
    void resizeTerminal(int, int) override {}
    bool isConnected() const override { return online; }
    QString errorString() const override { return {}; }
    bool online{false};
    QByteArray written;
};
struct Fixture {
    QTemporaryDir root{QDir::currentPath() + "/novaterm-text-wait-XXXXXX"};
    TerminalCore core{80, 2};
    OfflineTransport transport;
    TerminalSession session{&core};
    SessionDirectory directory;
    AccessStore access{root.path(), std::make_unique<MemoryCredentialStore>(), nullptr,
        [] { return std::make_unique<MemoryCredentialStore>(); }};
    qint64 now{0};
    ObservationService observation{[this] { return now; }};
    QByteArray key{randomBytes()};
    bool logBusy{false};
    QString sourceOverride;
    int prefixBytes{0};
    QString lastReadToken;
    std::function<void(const ReadContextRequest&)> beforeLogRead;
    TerminalTextWait waiter{access, directory, observation, "test", key, nullptr, [this] { return now; },
        [this](const QString& principal, const ReadContextRequest& request) -> Result<QJsonObject> {
            if (beforeLogRead) beforeLogRead(request);
            if (logBusy) return Error{ErrorCode::Busy};
            lastReadToken = request.sinceToken.value_or(QString{});
            auto limited = request;
            if (prefixBytes > 0) limited.maxBytes = std::min(prefixBytes, request.maxBytes.value_or(prefixBytes));
            auto result = readLogContext(access, directory, "test", principal, key, limited);
            if (auto* page = std::get_if<QJsonObject>(&result); page && !sourceOverride.isEmpty()
                && page->value("status") == "ok" && !page->value("fragments").toArray().isEmpty()) {
                page->insert("status", sourceOverride);
                page->insert("caughtUp", false);
                page->insert("hasMore", true);
            }
            return result;
        }};
    QString client;
    Fixture() {
        if (!root.isValid()) qFatal("test temp directory unavailable: %s (temp=%s)",
            qPrintable(root.errorString()), qPrintable(QDir::tempPath()));
        session.attach(&transport, TerminalSession::Ownership::Borrowed, TransportKind::LocalShell);
        if (!session.start()) qFatal("test session failed");
        directory.add(&session);
        if (!access.setEnabled(true)) qFatal("test access store unavailable");
        client = access.ensureAccess();
        if (!TestSupport::setGrant(access, client, entry(), true)) qFatal("test read grant failed");
        if (!core.waitForIdle()) qFatal("parser failed to idle");
    }
    SessionDirectory::Entry entry() { return directory.entries().first(); }
    WaitTextRequest request(QStringList texts = {"PASS"}) {
        WaitTextRequest value;
        value.session = NovaTerm::Mcp::SessionId{entry().id}; value.epoch = Epoch{entry().epoch}; value.texts = std::move(texts);
        return value;
    }
    QString bookmark(LogInitialPosition start = LogInitialPosition::Oldest) {
        ReadContextRequest read;
        read.session = NovaTerm::Mcp::SessionId{entry().id}; read.epoch = Epoch{entry().epoch};
        read.source = ContextSource::Log; read.start = start;
        // 最早位置票据独立于本次基线页实际交付的文本。
        const auto result = readLogContext(access, directory, "test", client, key, read);
        if (isError(result)) qFatal("test bookmark failed");
        return std::get<QJsonObject>(result).value(start == LogInitialPosition::Oldest ? "earliestToken" : "nextToken").toString();
    }
    void output(const QByteArray& bytes) {
        if (!core.writeInput(bytes).fullyAccepted() || !core.waitForIdle()) qFatal("parser test output failed");
        now += 300; // 确定性地越过 ObservationService 合并窗口。
    }
};
QJsonObject data(const QJsonObject& result) { return result.value("data").toObject(); }
}
class TextWaitTests final : public QObject {
    Q_OBJECT
private slots:
    void fragmentedUtf8AndHardBreak() {
        TextWaitMatcher matcher({QStringLiteral("\u542f\u52a8PASS")}, WaitTextMatch::Any, true);
        QVERIFY(!matcher.feed(QStringLiteral("\u542f\u52a8PA")));
        QVERIFY(matcher.feed("SS\n"));
        TextWaitMatcher separate({"PASS"}, WaitTextMatch::Any, true);
        QVERIFY(!separate.feed("PA\n"));
        QVERIFY(!separate.feed("SS\n"));
    }
    void allAndCaseOptions() {
        TextWaitMatcher matcher({"ready", "pass"}, WaitTextMatch::All, false);
        QVERIFY(!matcher.feed("READY\n"));
        QCOMPARE(matcher.matchedIndices().size(), 1);
        QVERIFY(matcher.feed("PASS\n"));
        TextWaitMatcher strict({"pass"}, WaitTextMatch::Any, true);
        QVERIFY(!strict.feed("PASS"));
    }
    void stateRetainsBoundedTail() {
        TextWaitMatcher matcher({"ABCDEF", "OK"}, WaitTextMatch::All, true);
        QVERIFY(!matcher.feed(QString(100000, 'x') + "OK\nABC"));
        QVERIFY(matcher.tail().size() <= 256);
        auto continued = matcher;
        QVERIFY(continued.feed("DEF"));
        QCOMPARE(continued.matchedIndices().size(), 2);
    }
    void defaultTailNeverMatchesPreviousPass() {
        Fixture f; f.output("PASS\r\nold\r\nend\r\n");
        std::optional<QJsonObject> result;
        f.waiter.call(f.client, "old", f.request(), [&result](auto value) { result = value; });
        QTRY_VERIFY(result); QVERIFY(result->value("ok").toBool());
        QCOMPARE(data(*result).value("status").toString(), "timeout");
        QVERIFY(!data(*result).value("nextToken").toString().isEmpty());
        QVERIFY(f.transport.written.isEmpty());
    }
    void logSinceMatchesNewCommittedTextAndPosition() {
        Fixture f; const auto start = f.bookmark(LogInitialPosition::Tail);
        f.output("PASS\r\nnext\r\nlast\r\n");
        auto request = f.request(); request.sinceToken = start;
        std::optional<QJsonObject> result;
        f.waiter.call(f.client, "new", request, [&result](auto value) { result = value; });
        QTRY_VERIFY(result); QVERIFY2(result->value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(*result).toJson())));
        QCOMPARE(data(*result).value("status").toString(), "matched");
        QVERIFY(!data(*result).value("provesSuccess").toBool());
        const auto evidence = data(*result).value("matchedTexts").toArray().first().toObject();
        QVERIFY(!evidence.value("endPosition").toObject().isEmpty());
        QVERIFY(evidence.value("context").toString().contains("PASS"));
        QVERIFY(f.transport.written.isEmpty());
    }
    void budgetResumePreservesPartialAndAllFacts() {
        Fixture f; auto request = f.request({"OK", QStringLiteral("\u542f\u52a8PASS")}); request.match = WaitTextMatch::All;
        request.sinceToken = f.bookmark(LogInitialPosition::Tail);
        f.output(QStringLiteral("OK\r\n\u542f\u52a8PASS\r\nnext\r\nlast\r\n").toUtf8());
        request.scanBytes = 11; // OK、LF、两个三字节标量和 PA。
        std::optional<QJsonObject> first;
        f.waiter.call(f.client, "page1", request, [&first](auto value) { first = value; });
        QTRY_VERIFY(first); QCOMPARE(data(*first).value("status").toString(), "scan_budget_exhausted");
        QCOMPARE(data(*first).value("scanBytes").toInt(), 11);
        QCOMPARE(data(*first).value("matchedTexts").toArray().size(), 1);
        request.sinceToken.reset(); request.resumeToken = data(*first).value("resumeToken").toString(); request.scanBytes = 64;
        std::optional<QJsonObject> second;
        f.waiter.call(f.client, "page2", request, [&second](auto value) { second = value; });
        QTRY_VERIFY(second); QCOMPARE(data(*second).value("status").toString(), "matched");
        QCOMPARE(data(*second).value("matchedTexts").toArray().size(), 2);
        QCOMPARE(data(*second).value("startToken"), data(*first).value("startToken"));
    }
    void hardNewlineCannotCreateFalsePass() {
        Fixture f; auto request = f.request(); request.sinceToken = f.bookmark(LogInitialPosition::Tail);
        f.output("PA\r\nSS\r\nnext\r\nlast\r\n");
        std::optional<QJsonObject> result;
        f.waiter.call(f.client, "break", request, [&result](auto value) { result = value; });
        QTRY_VERIFY(result); QCOMPARE(data(*result).value("status").toString(), "timeout");
    }
    void screenCurrentMatchesUnterminatedPrompt() {
        Fixture f; f.output("U-Boot> ");
        auto request = f.request({"U-Boot>"}); request.source = WaitTextSource::Screen;
        std::optional<QJsonObject> result;
        f.waiter.call(f.client, "screen", request, [&result](auto value) { result = value; });
        QTRY_VERIFY(result); QCOMPARE(data(*result).value("status").toString(), "matched");
        QVERIFY(!data(*result).value("revision").toString().isEmpty());
        QVERIFY(!data(*result).value("capturedAt").toString().isEmpty());
        QVERIFY(!data(*result).contains("nextToken"));
        QCOMPARE(f.observation.captureCount(), size_t{0});
    }
    void screenNextIgnoresBaselineAndNeverJoinsRevisions() {
        Fixture f; f.output("PASS");
        auto request = f.request(); request.source = WaitTextSource::Screen; request.screenStart = ScreenWaitStart::Next;
        std::optional<QJsonObject> first;
        f.waiter.call(f.client, "baseline", request, [&first](auto value) { first = value; });
        QTRY_VERIFY(first); QCOMPARE(data(*first).value("status").toString(), "timeout");
        request.resumeToken = data(*first).value("resumeToken").toString();
        f.output("\rPA\x1b[K");
        std::optional<QJsonObject> second;
        f.waiter.call(f.client, "pa", request, [&second](auto value) { second = value; });
        QTRY_VERIFY(second); QCOMPARE(data(*second).value("status").toString(), "timeout");
        request.resumeToken = data(*second).value("resumeToken").toString();
        f.output("\rSS\x1b[K");
        std::optional<QJsonObject> third;
        f.waiter.call(f.client, "ss", request, [&third](auto value) { third = value; });
        QTRY_VERIFY(third); QCOMPARE(data(*third).value("status").toString(), "timeout");
    }
    void cancellationAndRevocationSendNoInput() {
        Fixture f; auto request = f.request(); request.wait = std::chrono::milliseconds{30000};
        std::optional<QJsonObject> cancelled;
        f.waiter.call(f.client, "cancel", request, [&cancelled](auto value) { cancelled = value; });
        QVERIFY(!cancelled); f.waiter.cancel("cancel"); QVERIFY(cancelled);
        QCOMPARE(data(*cancelled).value("status").toString(), "cancelled");
        std::optional<QJsonObject> revoked;
        f.waiter.call(f.client, "revoke", request, [&revoked](auto value) { revoked = value; });
        QVERIFY(!revoked); QVERIFY(TestSupport::setGrant(f.access, f.client, f.entry(), false));
        QTRY_VERIFY(revoked); QCOMPARE(revoked->value("error").toObject().value("code").toString(), "SESSION_NOT_AVAILABLE");
        QVERIFY(f.transport.written.isEmpty());
    }
    void clearInvalidatesActiveAndRetainedWaits() {
        Fixture f; auto request = f.request(); request.wait = std::chrono::milliseconds{30000};
        std::optional<QJsonObject> active;
        f.waiter.call(f.client, "clear", request, [&active](auto value) { active = value; });
        QVERIFY(!active); QVERIFY(f.core.clearScrollback()); QVERIFY(f.core.waitForIdle());
        QTRY_VERIFY(active); QCOMPARE(data(*active).value("status").toString(), "invalidated");
        request.wait = std::chrono::milliseconds{0};
        std::optional<QJsonObject> retained;
        f.waiter.call(f.client, "retain", request, [&retained](auto value) { retained = value; });
        QTRY_VERIFY(retained);
        request.resumeToken = data(*retained).value("resumeToken").toString();
        QVERIFY(f.core.clearScrollback()); QVERIFY(f.core.waitForIdle());
        std::optional<QJsonObject> resumed;
        f.waiter.call(f.client, "old-generation", request, [&resumed](auto value) { resumed = value; });
        QTRY_VERIFY(resumed); QCOMPARE(data(*resumed).value("status").toString(), "invalidated");
        QVERIFY(f.transport.written.isEmpty());
    }
    void continuationBoundToConditionAndRejectsForgedPrincipal() {
        Fixture f; auto request = f.request();
        std::optional<QJsonObject> first;
        f.waiter.call(f.client, "first", request, [&first](auto value) { first = value; });
        QTRY_VERIFY(first); request.resumeToken = data(*first).value("resumeToken").toString(); request.texts = {"FAIL"};
        std::optional<QJsonObject> mismatch;
        f.waiter.call(f.client, "mismatch", request, [&mismatch](auto value) { mismatch = value; });
        QVERIFY(mismatch); QCOMPARE(mismatch->value("error").toObject().value("code").toString(), "CURSOR_STALE");
        request.texts = {"PASS"}; const auto other = newId();
        QCOMPARE(f.access.identityVersion(other), quint64(0));
        QVERIFY(!TestSupport::setGrant(f.access, other, f.entry(), true));
        std::optional<QJsonObject> wrongClient;
        f.waiter.call(other, "other", request, [&wrongClient](auto value) { wrongClient = value; });
        QVERIFY(wrongClient); QCOMPARE(wrongClient->value("error").toObject().value("code").toString(), "SESSION_NOT_AVAILABLE");
    }
    void retainedStateCapacityNeverSilentlyResets() {
        Fixture f;
        for (int i = 0; i < 8; ++i) {
            std::optional<QJsonObject> result;
            f.waiter.call(f.client, QString::number(i), f.request(), [&result](auto value) { result = value; });
            QTRY_VERIFY(result); QVERIFY(result->value("ok").toBool());
        }
        std::optional<QJsonObject> full;
        f.waiter.call(f.client, "full", f.request(), [&full](auto value) { full = value; });
        QVERIFY(full); QCOMPARE(full->value("error").toObject().value("code").toString(), "BUSY");
        QVERIFY(f.transport.written.isEmpty());
    }
    void continuationExpiresAtFixedTenMinuteDeadline() {
        Fixture f; auto request = f.request();
        std::optional<QJsonObject> first;
        f.waiter.call(f.client, "first", request, [&first](auto value) { first = value; });
        QTRY_VERIFY(first); request.resumeToken = data(*first).value("resumeToken").toString();
        f.now = 599999;
        std::optional<QJsonObject> almost;
        f.waiter.call(f.client, "almost", request, [&almost](auto value) { almost = value; });
        QTRY_VERIFY(almost); QCOMPARE(data(*almost).value("resumeExpiresInMs").toInt(), 1);
        request.resumeToken = data(*almost).value("resumeToken").toString();
        f.now = 600000;
        std::optional<QJsonObject> expired;
        f.waiter.call(f.client, "expired", request, [&expired](auto value) { expired = value; });
        QVERIFY(expired); QCOMPARE(expired->value("error").toObject().value("code").toString(), "CURSOR_STALE");
    }
    void actualLogPagesPreserveLiteralAtBoundary() {
        Fixture f; auto request = f.request({QStringLiteral("\u542f\u52a8PASS")});
        request.sinceToken = f.bookmark(LogInitialPosition::Tail);
        f.output(QByteArray(16382, 'x') + QStringLiteral("\u542f\u52a8PASS\r\nnext\r\nlast\r\n").toUtf8());
        std::optional<QJsonObject> result;
        f.waiter.call(f.client, "pages", request, [&result](auto value) { result = value; });
        QTRY_VERIFY(result); QCOMPARE(data(*result).value("status").toString(), "matched");
        QVERIFY(data(*result).value("scanBytes").toInt() > 16384);
        QVERIFY(data(*result).value("context").toString().toUtf8().size() <= 2048);
        QVERIFY(f.transport.written.isEmpty());
    }
    void scalarBudgetExhaustionKeepsExactPosition() {
        Fixture f; auto request = f.request({QStringLiteral("\U0001f600")});
        request.sinceToken = f.bookmark(LogInitialPosition::Tail);
        f.output(QStringLiteral("\U0001f600\r\nnext\r\nlast\r\n").toUtf8()); request.scanBytes = 1;
        std::optional<QJsonObject> first;
        f.waiter.call(f.client, "one-byte", request, [&first](auto value) { first = value; });
        QTRY_VERIFY(first); QCOMPARE(data(*first).value("status").toString(), "scan_budget_exhausted");
        QCOMPARE(data(*first).value("nextToken").toString(), *request.sinceToken);
        QCOMPARE(data(*first).value("scanBytes").toInt(), 0);
        request.sinceToken.reset(); request.resumeToken = data(*first).value("resumeToken").toString(); request.scanBytes = 4;
        std::optional<QJsonObject> second;
        f.waiter.call(f.client, "four-bytes", request, [&second](auto value) { second = value; });
        QTRY_VERIFY(second); QCOMPARE(data(*second).value("status").toString(), "matched");
    }
    void evictionReportsGapWithoutNewBaseline() {
        Fixture f; auto request = f.request({"NOT_PRESENT"});
        request.sinceToken = f.bookmark(LogInitialPosition::Tail);
        f.output("old\r\nnext\r\nlast\r\n"); request.scanBytes = 1;
        std::optional<QJsonObject> first;
        f.waiter.call(f.client, "first-byte", request, [&first](auto value) { first = value; });
        QTRY_VERIFY(first); QCOMPARE(data(*first).value("status").toString(), "scan_budget_exhausted");
        request.sinceToken.reset(); request.resumeToken = data(*first).value("resumeToken").toString(); request.scanBytes = 1024;
        f.core.setScrollbackLimit(2); QVERIFY(f.core.waitForIdle());
        f.output("1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n");
        std::optional<QJsonObject> gap;
        f.waiter.call(f.client, "evicted", request, [&gap](auto value) { gap = value; });
        QTRY_VERIFY(gap); QCOMPARE(data(*gap).value("status").toString(), "gap");
        QVERIFY(!data(*gap).value("gap").toObject().value("earliestToken").toString().isEmpty());
        QCOMPARE(data(*gap).value("nextToken"), data(*first).value("nextToken"));
        QVERIFY(!data(*gap).contains("resumeToken"));
    }
    void screenAllRequiresOneRevision() {
        Fixture f; f.output("READY"); auto request = f.request({"READY", "PASS"});
        request.source = WaitTextSource::Screen; request.match = WaitTextMatch::All;
        std::optional<QJsonObject> first;
        f.waiter.call(f.client, "ready", request, [&first](auto value) { first = value; });
        QTRY_VERIFY(first); QCOMPARE(data(*first).value("status").toString(), "timeout");
        request.resumeToken = data(*first).value("resumeToken").toString(); f.output("\rPASS\x1b[K");
        std::optional<QJsonObject> second;
        f.waiter.call(f.client, "pass", request, [&second](auto value) { second = value; });
        QTRY_VERIFY(second); QCOMPARE(data(*second).value("status").toString(), "timeout");
        QCOMPARE(data(*second).value("matchedTexts").toArray().size(), 1);
    }
    void screenBudgetRetainsRevisionAndResumesAtomically() {
        Fixture f; f.output("U-Boot>"); auto request = f.request({"U-Boot>"});
        request.source = WaitTextSource::Screen; request.scanBytes = 1;
        std::optional<QJsonObject> first;
        f.waiter.call(f.client, "tiny-screen", request, [&first](auto value) { first = value; });
        QTRY_VERIFY(first); QCOMPARE(data(*first).value("status").toString(), "scan_budget_exhausted");
        QVERIFY(!data(*first).value("revision").toString().isEmpty());
        QCOMPARE(data(*first).value("scanBytes").toInt(), 0);
        request.resumeToken = data(*first).value("resumeToken").toString(); request.scanBytes = 1024;
        std::optional<QJsonObject> resumed;
        f.waiter.call(f.client, "full-screen", request, [&resumed](auto value) { resumed = value; });
        QTRY_VERIFY(resumed); QCOMPARE(data(*resumed).value("status").toString(), "matched");
        QCOMPARE(data(*resumed).value("revision"), data(*first).value("revision"));
    }
    void coldPagePendingRetainsCursorUntilCacheReady() {
        Fixture f;
        auto history = std::make_shared<NovaTerm::HistoryCompressionService>();
        NovaTerm::HistoryCompressionPolicy policy;
        policy.enabled = true; policy.hotBytes = 0;
        QVERIFY(f.core.configureHistory(history, policy)); QVERIFY(f.core.waitForIdle());
        auto request = f.request({"FIRST_NEEDLE"}); request.sinceToken = f.bookmark(LogInitialPosition::Tail);
        QByteArray text = "FIRST_NEEDLE\r\n";
        for (int i = 0; i < 2500; ++i) text += QByteArray::number(i) + QByteArray(65, 'x') + "\r\n";
        f.output(text);
        QTRY_VERIFY_WITH_TIMEOUT(f.core.scrollbackStatistics().compressedBlocks > 0, 5000);
        std::optional<QJsonObject> first;
        f.waiter.call(f.client, "cold", request, [&first](auto value) { first = value; });
        QTRY_VERIFY(first);
        QCOMPARE(data(*first).value("status").toString(), "timeout");
        QCOMPARE(data(*first).value("sourceStatus").toString(), "pending");
        QCOMPARE(data(*first).value("nextToken").toString(), *request.sinceToken);
        QCOMPARE(data(*first).value("scanBytes").toInt(), 0);
        request.sinceToken.reset(); request.resumeToken = data(*first).value("resumeToken").toString();
        request.wait = std::chrono::milliseconds{30000};
        std::optional<QJsonObject> resumed;
        f.waiter.call(f.client, "decoded", request, [&resumed](auto value) { resumed = value; });
        QTRY_VERIFY_WITH_TIMEOUT(resumed, 5000);
        QCOMPARE(data(*resumed).value("status").toString(), "matched");
        QVERIFY(f.transport.written.isEmpty());
    }
    void staleEpochAndReconnectNeverReuseOldWait() {
        Fixture f; auto request = f.request(); auto stale = request; stale.epoch = Epoch{"wrong-epoch"};
        std::optional<QJsonObject> rejected;
        f.waiter.call(f.client, "stale", stale, [&rejected](auto value) { rejected = value; });
        QVERIFY(rejected); QCOMPARE(rejected->value("error").toObject().value("code").toString(), "STALE_SESSION_EPOCH");
        request.wait = std::chrono::milliseconds{30000};
        std::optional<QJsonObject> disconnected;
        f.waiter.call(f.client, "disconnect", request, [&disconnected](auto value) { disconnected = value; });
        QVERIFY(!disconnected);
        f.session.close(CloseMode::Abort);
        QTRY_VERIFY(disconnected);
        QVERIFY(data(*disconnected).value("status").toString() == "invalidated"
            || disconnected->value("error").toObject().value("code").toString() == "SESSION_NOT_AVAILABLE");
        QVERIFY(f.session.resetForReuse());
        f.session.attach(&f.transport, TerminalSession::Ownership::Borrowed, TransportKind::LocalShell);
        QVERIFY(f.session.start()); QVERIFY(f.core.waitForIdle());
        QVERIFY(f.entry().epoch != request.epoch.str());
        std::optional<QJsonObject> old;
        request.wait = std::chrono::milliseconds{0};
        f.waiter.call(f.client, "old-after-reconnect", request, [&old](auto value) { old = value; });
        QVERIFY(old); QVERIFY(!old->value("ok").toBool());
        QVERIFY(f.transport.written.isEmpty());
    }
    void activeDeadlineRetainsReadOnlyProgress() {
        Fixture f; auto request = f.request(); request.wait = std::chrono::milliseconds{30000};
        std::optional<QJsonObject> result;
        f.waiter.call(f.client, "deadline", request, [&result](auto value) { result = value; });
        QVERIFY(!result); f.now = 30000;
        QTRY_VERIFY(result); QCOMPARE(data(*result).value("status").toString(), "timeout");
        QVERIFY(!data(*result).value("nextToken").toString().isEmpty());
        QVERIFY(!data(*result).value("resumeToken").toString().isEmpty());
        QVERIFY(f.transport.written.isEmpty());
    }
    void slowPollVisitsEveryConcurrentWait() {
        // 捕获对象先于 Fixture 创建，失败退出时也保持到所有等待销毁之后。
        QVector<std::optional<QJsonObject>> results(8);
        QSet<int> visited;
        int slowReads = 0;
        Fixture f;
        // All eight connections use the same principal and share its bounded wait budget.
        for (int index = 0; index < 8; ++index) {
            auto request = f.request();
            request.wait = std::chrono::milliseconds{index % 2 == 0 ? 1000 : 30000};
            request.scanBytes = 100 + index;
            f.waiter.call(f.client, QString::number(index), request,
                [&results, index](auto value) { results[index] = value; });
            QVERIFY(!results[index]);
        }
        // 初次同步调用已建立全部基线。睡眠仅构造超过 8ms 的慢日志依赖，
        // 不处理事件或等待竞争；单调时钟固定，八条等待在观察轮转时保持稳定。
        f.beforeLogRead = [&visited, &slowReads](const ReadContextRequest& request) {
            QTest::qSleep(9);
            visited.insert(request.maxBytes.value_or(0));
            ++slowReads;
        };
        QTRY_VERIFY_WITH_TIMEOUT(slowReads >= 16, 3000);
        QCOMPARE(visited.size(), 8);
        for (int index = 0; index < 8; ++index) QVERIFY(visited.contains(100 + index));
        f.beforeLogRead = {};
        f.now = 1000;
        for (int index = 0; index < 8; index += 2) {
            QTRY_VERIFY(results[index]);
            QCOMPARE(data(*results[index]).value("status").toString(), "timeout");
            QVERIFY(!data(*results[index]).value("nextToken").toString().isEmpty());
            QVERIFY(!data(*results[index]).value("resumeToken").toString().isEmpty());
        }
        f.output("PASS\r\nnext\r\nlast\r\n");
        for (int index = 1; index < 8; index += 2) {
            QTRY_VERIFY(results[index]);
            QCOMPARE(data(*results[index]).value("status").toString(), "matched");
        }
        QVERIFY(f.transport.written.isEmpty());
    }
    void invalidatedWaitNeverReturnsOldGenerationEvidence() {
        Fixture f; auto request = f.request({"PASS", "UNSEEN"}); request.match = WaitTextMatch::All;
        request.sinceToken = f.bookmark(LogInitialPosition::Tail); request.scanBytes = 5;
        f.output("PASS\r\nnext\r\nlast\r\n");
        std::optional<QJsonObject> first;
        f.waiter.call(f.client, "evidence", request, [&first](auto value) { first = value; });
        QTRY_VERIFY(first); QCOMPARE(data(*first).value("status").toString(), "scan_budget_exhausted");
        QCOMPARE(data(*first).value("matchedTexts").toArray().size(), 1);
        QVERIFY(data(*first).value("context").toString().contains("PASS"));
        request.sinceToken.reset(); request.resumeToken = data(*first).value("resumeToken").toString(); request.scanBytes = 1024;
        QVERIFY(f.core.clearScrollback()); QVERIFY(f.core.waitForIdle());
        std::optional<QJsonObject> cleared;
        f.waiter.call(f.client, "cleared-evidence", request, [&cleared](auto value) { cleared = value; });
        QTRY_VERIFY(cleared); QCOMPARE(data(*cleared).value("status").toString(), "invalidated");
        QVERIFY(data(*cleared).value("context").toString().isEmpty());
        QVERIFY(data(*cleared).value("matchedTexts").toArray().isEmpty());
        QCOMPARE(data(*cleared).value("nextToken"), data(*first).value("nextToken"));
        QVERIFY(!data(*cleared).value("gap").toObject().value("earliestToken").toString().isEmpty());
        QVERIFY(!data(*cleared).contains("resumeToken"));
        QVERIFY(f.transport.written.isEmpty());
    }
    void discardedRpcWaitsNeverOrphanRetainedQuota() {
        Fixture f; auto request = f.request(); request.wait = std::chrono::milliseconds{30000};
        int callbacks = 0;
        for (int i = 0; i < 40; ++i) {
            const auto id = QString::number(i);
            f.waiter.call(f.client, id, request, [&callbacks](auto) { ++callbacks; });
            QCOMPARE(callbacks, 0);
            f.waiter.discard(id);
            QCOMPARE(callbacks, 0);
        }
        std::optional<QJsonObject> next;
        f.waiter.call(f.client, "after-discard", request, [&next](auto value) { next = value; });
        QVERIFY(!next); f.waiter.cancel("after-discard"); QVERIFY(next);
        QCOMPARE(data(*next).value("status").toString(), "cancelled");
        QVERIFY(!data(*next).value("resumeToken").toString().isEmpty());
        QVERIFY(f.transport.written.isEmpty());
    }
    void busyResumeRetainsPartialAllFactsAndExactPosition() {
        Fixture f; auto request = f.request({"PASS", "DONE"}); request.match = WaitTextMatch::All;
        request.sinceToken = f.bookmark(LogInitialPosition::Tail); request.scanBytes = 5;
        f.output("PASS\r\nnext\r\nlast\r\n");
        std::optional<QJsonObject> first;
        f.waiter.call(f.client, "partial", request, [&first](auto value) { first = value; });
        QTRY_VERIFY(first); QCOMPARE(data(*first).value("matchedTexts").toArray().size(), 1);
        request.sinceToken.reset(); request.resumeToken = data(*first).value("resumeToken").toString(); request.scanBytes = 1024;
        f.logBusy = true;
        std::optional<QJsonObject> busy;
        f.waiter.call(f.client, "busy", request, [&busy](auto value) { busy = value; });
        QTRY_VERIFY(busy); QVERIFY(busy->value("ok").toBool());
        QCOMPARE(data(*busy).value("status").toString(), "timeout");
        QCOMPARE(data(*busy).value("sourceStatus").toString(), "busy");
        QCOMPARE(data(*busy).value("nextToken"), data(*first).value("nextToken"));
        QCOMPARE(data(*busy).value("matchedTexts").toArray().size(), 1);
        QCOMPARE(data(*busy).value("scanBytes").toInt(), 0);
        request.resumeToken = data(*busy).value("resumeToken").toString(); f.logBusy = false;
        f.output("DONE\r\nnext\r\nlast\r\n");
        std::optional<QJsonObject> final;
        f.waiter.call(f.client, "complete-all", request, [&final](auto value) { final = value; });
        QTRY_VERIFY(final); QCOMPARE(data(*final).value("status").toString(), "matched");
        QCOMPARE(data(*final).value("matchedTexts").toArray().size(), 2);
        QVERIFY(f.transport.written.isEmpty());
    }
    void busyWithoutInitialBaselineIsExplicitRetryableFailure() {
        Fixture f; f.logBusy = true;
        std::optional<QJsonObject> first;
        f.waiter.call(f.client, "no-baseline", f.request(), [&first](auto value) { first = value; });
        QVERIFY(first); QCOMPARE(first->value("error").toObject().value("code").toString(), "BUSY");
        QVERIFY(first->value("error").toObject().value("retryable").toBool());
        QVERIFY(!first->contains("data"));
        f.logBusy = false;
        std::optional<QJsonObject> retried;
        f.waiter.call(f.client, "retry-baseline", f.request(), [&retried](auto value) { retried = value; });
        QTRY_VERIFY(retried); QCOMPARE(data(*retried).value("status").toString(), "timeout");
    }
    void sourcePrefixMatchesBeforeBlockedPage_data() {
        QTest::addColumn<QString>("sourceStatus");
        QTest::newRow("pending") << QStringLiteral("pending");
        QTest::newRow("busy") << QStringLiteral("busy");
        QTest::newRow("unavailable") << QStringLiteral("unavailable");
    }
    void sourcePrefixMatchesBeforeBlockedPage() {
        QFETCH(QString, sourceStatus);
        Fixture f; auto request = f.request(); request.sinceToken = f.bookmark(LogInitialPosition::Tail);
        f.output("PASS\r\nnext\r\nlast\r\n"); f.prefixBytes = 5; f.sourceOverride = sourceStatus;
        std::optional<QJsonObject> result;
        f.waiter.call(f.client, "prefix-match", request, [&result](auto value) { result = value; });
        QTRY_VERIFY(result); QCOMPARE(data(*result).value("status").toString(), "matched");
        QCOMPARE(data(*result).value("sourceStatus").toString(), sourceStatus);
        QCOMPARE(data(*result).value("scanBytes").toInt(), 5);
        QVERIFY(data(*result).value("nextToken").toString() != *request.sinceToken);
        QVERIFY(f.transport.written.isEmpty());
    }
    void sourcePrefixContinuationConsumesExactlyOnce_data() {
        sourcePrefixMatchesBeforeBlockedPage_data();
    }
    void sourcePrefixContinuationConsumesExactlyOnce() {
        QFETCH(QString, sourceStatus);
        Fixture f; auto request = f.request({"READY", "PASS"}); request.match = WaitTextMatch::All;
        request.sinceToken = f.bookmark(LogInitialPosition::Tail);
        f.output("READY\r\nPASS\r\nnext\r\nlast\r\n"); f.prefixBytes = 8; f.sourceOverride = sourceStatus;
        std::optional<QJsonObject> first;
        f.waiter.call(f.client, "prefix-partial", request, [&first](auto value) { first = value; });
        QTRY_VERIFY(first); QCOMPARE(data(*first).value("status").toString(), "timeout");
        QCOMPARE(data(*first).value("sourceStatus").toString(), sourceStatus);
        QCOMPARE(data(*first).value("scanBytes").toInt(), 8);
        QCOMPARE(data(*first).value("matchedTexts").toArray().size(), 1);
        QVERIFY(data(*first).value("nextToken").toString() != *request.sinceToken);
        request.sinceToken.reset(); request.resumeToken = data(*first).value("resumeToken").toString();
        f.sourceOverride.clear(); f.prefixBytes = 0;
        std::optional<QJsonObject> resumed;
        f.waiter.call(f.client, "prefix-rest", request, [&resumed](auto value) { resumed = value; });
        QTRY_VERIFY(resumed); QCOMPARE(data(*resumed).value("status").toString(), "matched");
        QCOMPARE(data(*resumed).value("matchedTexts").toArray().size(), 2);
        QCOMPARE(f.lastReadToken, data(*first).value("nextToken").toString());
        QCOMPARE(data(*resumed).value("startToken"), data(*first).value("startToken"));
        QVERIFY(f.transport.written.isEmpty());
    }
};
QTEST_GUILESS_MAIN(TextWaitTests)
#include "TextWaitTests.moc"
