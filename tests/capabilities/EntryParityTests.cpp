#include "capabilities/terminal/ReadCapabilities.h"
#include "capabilities/terminal/InputCapabilities.h"
#include "capabilities/terminal/ObservationCapabilities.h"
#include "capabilities/terminal/SearchCapabilities.h"
#include "capabilities/McpCapabilityAdapter.h"
#include <QtTest>
namespace C = NovaTerm::Capabilities;
namespace M = NovaTerm::Mcp;
namespace {
class ReadSessions final : public M::ReadSessionPort
{
public:
    mutable M::ClientId observed;
    quint64 directoryRevision() const override { return 1; }
    M::ClientAccessVersion accessVersion(const M::ClientId&) const override { return M::ClientAccessVersion{1}; }
    QList<M::ReadSession> pageAfter(const M::ClientId& client, const M::SessionId&, int, bool) const override {
        observed = client;
        M::ReadSession row;
        row.reference = {M::SessionId{"session"}, M::Epoch{"epoch"}};
        row.displayName = "target"; row.running = true;
        return {row};
    }
    M::Result<M::ContextSummary> readContext(const M::ContextQuery& request) const override {
        observed = request.client;
        M::ContextSummary summary; summary.viewport = {"hello"}; summary.revision = 2;
        return summary;
    }
};
}
class EntryParityTests : public QObject
{
    Q_OBJECT
private slots:
    void observationAndSearchUseTheSameAdmission()
    {
        auto registry = C::CapabilityRegistry::terminalCapabilities();
        C::CapabilityAuthorization auth{[](const C::PrincipalId&, C::EntryKind, const C::ResourceBinding& resource,
            const C::Definition&) { return std::optional<C::AuthorizationSnapshot>{{resource, 1}}; }};
        C::CapabilityDispatcher dispatcher{registry, auth};
        C::McpCapabilityAdapter second{auth, C::PrincipalId{"client"}};
        C::McpCapabilityAdapter mcp{auth, C::PrincipalId{"client"}};
        C::SessionBinding binding{C::SessionId{"session"}, C::AttachmentId{"attach"}, C::Epoch{"epoch"}, 1, {}};
        M::ObservationState state{[] { return 100; }};
        C::Terminal::ObservationCapabilities observations{dispatcher, state};
        M::ObservationWait wait;
        wait.client = M::ClientId{"forged"}; wait.session = {M::SessionId{"session"}, M::Epoch{"epoch"}};
        wait.request = M::RequestId{"request"}; wait.initial = true;
        M::ReadContextRequest read; read.session = wait.session.session; read.epoch = wait.session.epoch;
        wait.typed = read;
        int completions = 0;
        second.invoke(observations.read, wait, binding, 1, C::OperationId{"observe"}, C::Clock::now() + std::chrono::seconds(5),
            [&completions](auto result) {
                const auto& completed = std::get<M::ObservationCompletion>(result);
                QCOMPARE(completed.wait.client.str(), QString("client")); ++completions;
            });
        QCOMPARE(state.waiterCount(), 1);
        M::ObservationFacts facts; facts.available = facts.snapshotAvailable = true;
        facts.session = wait.session; facts.grant = M::ClientAccessVersion{1};
        auto completed = state.evaluate(wait.request, facts);
        QVERIFY(completed); observations.complete(*completed);
        QCOMPARE(completions, 1); QCOMPARE(state.waiterCount(), 0);
        M::CaptureRecord record; record.scope = M::CaptureScope::Client; record.owner = "client";
        record.session = "session"; record.epoch = "epoch"; record.grantVersion = 1;
        record.lines = {{"needle", "viewport", 0}};
        C::Terminal::SearchCapabilities searches{dispatcher,
            [record](const auto&, const auto&) -> M::Result<M::CaptureRecord> { return record; },
            [](M::CaptureRecord capture, M::SearchContextRequest request, auto done) {
                done(M::searchCapture(capture, request.query, true, "both", 20));
            }};
        M::SearchContextRequest query{M::SessionId{"session"}, M::Epoch{"epoch"}, M::CaptureId{"capture"}, "needle"};
        const auto done = [](auto result) {
            const auto& value = std::get<C::Terminal::ContextSearchResult>(std::get<M::Result<C::Terminal::ContextSearchResult>>(result));
            QCOMPARE(value.search.matches.size(), 1); QCOMPARE(value.search.matches.front().startByte, 0);
        };
        second.invoke(searches.search, query, binding, 1, C::OperationId{"second-search"}, C::Clock::now() + std::chrono::seconds(5), done);
        mcp.invoke(searches.search, query, binding, 1, C::OperationId{"mcp-search"}, C::Clock::now() + std::chrono::seconds(5), done);
        wait.request = M::RequestId{"cancelled"};
        second.invoke(observations.read, wait, binding, 1, C::OperationId{"cancel-observe"}, C::Clock::now() + std::chrono::seconds(5), [](auto) { QFAIL("cancelled observation delivered"); });
        QVERIFY(dispatcher.cancel(C::PrincipalId{"client"}, C::OperationId{"cancel-observe"}));
        QCOMPARE(state.waiterCount(), 0);
    }
    void inputPortsShareReplayAndRecheckAfterAcquisition()
    {
        class Input final : public C::Terminal::TerminalInputPort {
        public:
            M::TerminalControlFacts current;
            int submissions{0};
            std::function<void()> acquired;
            M::TerminalControlFacts facts(const M::ClientId&, const M::SessionReference&) override { return current; }
            bool acquire(const M::ClientId&, const M::SessionReference&) override { if (acquired) acquired(); return true; }
            bool submit(const M::ClientId&, const M::InteractTerminalRequest&) override { ++submissions; return true; }
            void release(const M::ClientId&, const M::InteractTerminalRequest&) override {}
            bool canElevate(const M::ClientId&, const M::SessionReference&) override { return false; }
        } source;
        bool allowed = true;
        auto registry = C::CapabilityRegistry::terminalCapabilities();
        C::CapabilityAuthorization auth{[&allowed](const C::PrincipalId&, C::EntryKind,
            const C::ResourceBinding& resource, const C::Definition&) -> std::optional<C::AuthorizationSnapshot> {
            if (!allowed) return std::nullopt;
            return C::AuthorizationSnapshot{resource, 1};
        }};
        C::CapabilityDispatcher dispatcher{registry, auth};
        M::ObservationState state{[] { return 100; }};
        C::Terminal::InputCapabilities ports{dispatcher, state, source};
        C::McpCapabilityAdapter second{auth, C::PrincipalId{"client"}};
        C::McpCapabilityAdapter mcp{auth, C::PrincipalId{"client"}};
        source.current.session = {M::SessionId{"session"}, M::Epoch{"epoch"}};
        source.current.grant = M::ClientAccessVersion{1};
        C::SessionBinding binding{C::SessionId{"session"}, C::AttachmentId{"attach"}, C::Epoch{"epoch"}, 1, {}};
        C::Terminal::InputSubmission call;
        call.request.session = M::SessionId{"session"}; call.request.epoch = M::Epoch{"epoch"};
        call.request.operation = M::OperationId{"input-op"}; call.request.actions = {M::TextAction{"hello"}};
        call.claims = state.inputClaims(M::ClientId{"client"}, source.current);
        second.invoke(ports.input, call, binding, 1, C::OperationId{"second"}, C::Clock::now() + std::chrono::seconds(5),
            [](auto result) { QCOMPARE(std::get<M::InputAdmission>(std::get<M::Result<M::InputAdmission>>(result)), M::InputAdmission::Submit); });
        mcp.invoke(ports.input, call, binding, 1, C::OperationId{"mcp"}, C::Clock::now() + std::chrono::seconds(5),
            [](auto result) { QCOMPARE(std::get<M::InputAdmission>(std::get<M::Result<M::InputAdmission>>(result)), M::InputAdmission::Replay); });
        QCOMPARE(source.submissions, 1);
        auto oversized = call;
        oversized.request.actions = {M::TextAction{QString(16385, 'x')}};
        second.invoke(ports.input, oversized, binding, 1, C::OperationId{"oversized"}, C::Clock::now() + std::chrono::seconds(5),
            [](auto result) {
                const auto& domain = std::get<M::Result<M::InputAdmission>>(result);
                QCOMPARE(M::errorIfAny(domain)->code, M::ErrorCode::InvalidArgument);
            });
        QCOMPARE(source.submissions, 1);
        ++source.current.user;
        call.claims = state.inputClaims(M::ClientId{"client"}, source.current);
        call.request.operation = M::OperationId{"new-input"};
        source.acquired = [&allowed] { allowed = false; };
        second.invoke(ports.input, call, binding, 1, C::OperationId{"revoke-on-acquire"}, C::Clock::now() + std::chrono::seconds(5),
            [](auto result) { QVERIFY(std::holds_alternative<C::Rejection>(result)); });
        QCOMPARE(source.submissions, 1);
    }
    void readOnlyEntryParityAndIndependentSwitches()
    {
        auto registry = C::CapabilityRegistry::terminalCapabilities();
        C::CapabilityAuthorization auth{[](const C::PrincipalId&, C::EntryKind, const C::ResourceBinding& resource,
            const C::Definition&) { return std::optional<C::AuthorizationSnapshot>{{resource, 1}}; }};
        C::CapabilityDispatcher dispatcher{registry, auth};
        ReadSessions source;
        C::Terminal::ReadCapabilities ports{dispatcher, source};
        C::McpCapabilityAdapter mcp{auth, C::PrincipalId{"client"}};
        C::McpCapabilityAdapter second{auth, C::PrincipalId{"client"}};
        C::SessionBinding binding{C::SessionId{"session"}, C::AttachmentId{"attachment"}, C::Epoch{"epoch"}, 1, {}};
        C::Terminal::ContextReadQuery query;
        query.query.session = {M::SessionId{"session"}, M::Epoch{"epoch"}};
        query.query.client = M::ClientId{"model-forged"};
        auto read = [&](auto& entry, const QString& operation) {
            std::optional<C::Result<M::Result<M::ContextPage>>> result;
            entry.invoke(ports.context, query, binding, 1, C::OperationId{operation},
                C::Clock::now() + std::chrono::seconds(5), [&result](auto value) { result = std::move(value); });
            return result;
        };
        const auto external = read(mcp, "external");
        const auto secondResult = read(second, "second-connection");
        QVERIFY(external && secondResult);
        const auto& a = std::get<M::ContextPage>(std::get<M::Result<M::ContextPage>>(*external));
        const auto& b = std::get<M::ContextPage>(std::get<M::Result<M::ContextPage>>(*secondResult));
        QCOMPARE(a.summary.viewport, b.summary.viewport);
        QCOMPARE(a.summary.revision, b.summary.revision);
        QCOMPARE(source.observed.str(), QString("client"));
        mcp.setEnabled(false);
        QVERIFY(std::holds_alternative<C::Rejection>(*read(mcp, "disabled")));
        QVERIFY(std::holds_alternative<M::Result<M::ContextPage>>(*read(second, "still-enabled")));
        second.setEnabled(false);
        QVERIFY(std::holds_alternative<C::Rejection>(*read(second, "second-disabled")));
    }
};
QTEST_GUILESS_MAIN(EntryParityTests)
#include "EntryParityTests.moc"
