#include "mcp/application/ObservationState.h"
#include <QTest>
using namespace NovaTerm::Mcp;
class ObservationStateTests final : public QObject {
    Q_OBJECT
private slots:
    void inputReplayAndGenerations();
    void waitReplacementAndBudget();
    void deadlineAndControlWake();
    void outputQuietWindow();
    void cursorAndCaptureBinding();
};
void ObservationStateTests::inputReplayAndGenerations()
{
    qint64 now = 100;
    ObservationState state{[&] { return now; }};
    const ClientId client{"client"};
    TerminalControlFacts facts;
    facts.session = {SessionId{"session"}, Epoch{"epoch"}};
    facts.grant = ClientAccessVersion{2}; facts.control = 3;
    InteractTerminalRequest request;
    request.session = facts.session.session; request.epoch = facts.session.epoch;
    request.operation = OperationId{"op"}; request.actions = {TextAction{"text"}};
    const auto claims = state.inputClaims(client, facts);
    QCOMPARE(std::get<InputAdmission>(state.prepareInput(client, request, claims, facts)), InputAdmission::Submit);
    state.inputAccepted(client, request, facts);
    QCOMPARE(std::get<InputAdmission>(state.prepareInput(client, request, claims, facts)), InputAdmission::Replay);
    request.actions = {TextAction{"different"}};
    QCOMPARE(errorIfAny(state.prepareInput(client, request, claims, facts))->code, ErrorCode::InputSequenceConflict);
    request.operation = OperationId{"new"};
    QCOMPARE(errorIfAny(state.prepareInput(client, request, claims, facts))->code, ErrorCode::InputAlreadySubmitted);
    ++facts.user;
    QCOMPARE(state.validateInput(claims, client, facts)->code, ErrorCode::ControlInvalid);
    now = claims.expires;
    QCOMPARE(state.validateInput(claims, client, facts)->code, ErrorCode::InputTokenExpired);
}
void ObservationStateTests::waitReplacementAndBudget()
{
    qint64 now = 0; ObservationState state{[&] { return now; }};
    ObservationWait wait;
    wait.client = ClientId{"client"}; wait.session = {SessionId{"session"}, Epoch{"epoch"}};
    wait.request = RequestId{"first"};
    QVERIFY(!isError(state.admit(wait)));
    wait.request = RequestId{"replacement"};
    const auto replaced = state.admit(wait);
    QCOMPARE(std::get<QList<ObservationCompletion>>(replaced).size(), 1);
    QCOMPARE(state.waiterCount(), 1);
    for (int i = 1; i < Budget::MaxObservationWaiters; ++i) {
        wait.client = ClientId{QString::number(i)}; wait.request = RequestId{QString::number(i)};
        QVERIFY(!isError(state.admit(wait)));
    }
    wait.client = ClientId{"overflow"}; wait.request = RequestId{"overflow"};
    QCOMPARE(errorIfAny(state.admit(wait))->code, ErrorCode::Busy);
    const auto revoked = state.revoke(ClientId{}, SessionId{});
    QCOMPARE(revoked.size(), Budget::MaxObservationWaiters);
    QVERIFY(!state.hasWaiters());
}
void ObservationStateTests::deadlineAndControlWake()
{
    qint64 now = 0; ObservationState state{[&] { return now; }};
    ObservationWait wait;
    wait.client = ClientId{"c"}; wait.request = RequestId{"r"}; wait.session = {SessionId{"s"}, Epoch{"e"}};
    ReadContextRequest read; read.wait = std::chrono::milliseconds{500}; read.waitFor = WaitCondition::Completion;
    wait.typed = read;
    QVERIFY(!isError(state.admit(wait)));
    ObservationFacts facts; facts.available = true; facts.snapshotAvailable = true;
    facts.session = wait.session; facts.running = true;
    QVERIFY(!state.evaluate(wait.request, facts));
    now = 500;
    QCOMPARE(state.evaluate(wait.request, facts)->wake, ObservationWake::Deadline);
    QVERIFY(!isError(state.admit(wait)));
    ++facts.user;
    QCOMPARE(state.evaluate(wait.request, facts)->wake, ObservationWake::ControlChanged);
    QVERIFY(!state.hasWaiters());
}
void ObservationStateTests::outputQuietWindow()
{
    qint64 now = 0; ObservationState state{[&] { return now; }};
    ObservationWait wait;
    wait.client = ClientId{"c"}; wait.request = RequestId{"r"}; wait.session = {SessionId{"s"}, Epoch{"e"}};
    ReadContextRequest read; read.wait = std::chrono::milliseconds{1000}; read.waitFor = WaitCondition::Change;
    wait.typed = read;
    QVERIFY(!isError(state.admit(wait)));
    state.outputChanged(wait.session.session, 1);
    ObservationFacts facts; facts.available = facts.snapshotAvailable = facts.running = true;
    facts.session = wait.session; facts.model = 1;
    now = 149; QVERIFY(!state.evaluate(wait.request, facts));
    now = 150; QCOMPARE(state.evaluate(wait.request, facts)->wake, ObservationWake::OutputChanged);
}
void ObservationStateTests::cursorAndCaptureBinding()
{
    qint64 now = 100; ObservationState state{[&] { return now; }};
    const ClientId client{"c"}; const SessionReference session{SessionId{"s"}, Epoch{"e"}};
    ObservationCursor cursor{{true, client, session, ClientAccessVersion{1}}, 5, 2, true, true};
    QVERIFY(!isError(state.restoreCursor(cursor, client, session, ClientAccessVersion{1}, true)));
    QCOMPARE(errorIfAny(state.restoreCursor(cursor, client, session, ClientAccessVersion{1}, false))->code, ErrorCode::CursorStale);
    CaptureRecord record; record.scope = CaptureScope::Client; record.owner = "c";
    record.session = "s"; record.epoch = "e"; record.grantVersion = 1; record.expires = 101;
    QVERIFY(state.captureAvailable(record, client, session, ClientAccessVersion{1}));
    now = 101; QVERIFY(!state.captureAvailable(record, client, session, ClientAccessVersion{1}));
}
QTEST_GUILESS_MAIN(ObservationStateTests)
#include "ObservationStateTests.moc"
