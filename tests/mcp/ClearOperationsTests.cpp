#include "mcp/tasks/ClearOperations.h"
#include <QTest>
using namespace NovaTerm::Mcp;
class ClearOperationsTests final : public QObject {
    Q_OBJECT
private slots:
    void exactRetriesAndConflicts() {
        ClearOperations ops;
        ClearRequest request{SessionId{"s"}, Epoch{"e"}, OperationId{"op"}, false, 1, "ticket"};
        auto first = ops.begin("client", request, 4, 100, 0);
        QVERIFY(!isError(first));
        auto record = std::get<ClearOperations::Admission>(first).record;
        QVERIFY(std::get<ClearOperations::Admission>(first).fresh);
        auto again = ops.begin("client", request, 4, 100, 0);
        QVERIFY(!isError(again)); QVERIFY(!std::get<ClearOperations::Admission>(again).fresh);
        QCOMPARE(std::get<ClearOperations::Admission>(again).record, record);
        request.all = true;
        QVERIFY(isError(ops.begin("client", request, 4, 100, 0)));
        ops.finish(record->id, ClearCompletion{2});
        QVERIFY(record->result.has_value());
        QVERIFY(!isError(*record->result));
        ops.finish(record->id, Error{ErrorCode::Cancelled}); // late callback never overwrites
        QVERIFY(!isError(*record->result));
    }
    void expiredTicketsNeverReexecuteAfterCollection() {
        ClearOperations ops;
        ClearRequest request{SessionId{"s"}, Epoch{"e"}, OperationId{"op"}, false, 1, "ticket"};
        auto admitted = ops.begin("client", request, 4, 100, 0);
        QVERIFY(!isError(admitted)); auto record = std::get<ClearOperations::Admission>(admitted).record;
        ops.finish(record->id, ClearCompletion{2});
        ops.sweep(101); QCOMPARE(ops.size(), size_t(0));
        QVERIFY(isError(ops.begin("client", request, 4, 100, 101)));
    }
    void boundedRecordsAndCancellation() {
        ClearOperations ops;
        for (int i=0; i<32; ++i) {
            ClearRequest request{SessionId{"s"}, Epoch{"e"}, OperationId{QString::number(i)}, false, 1, "ticket"};
            QVERIFY(!isError(ops.begin("client", request, 4, 100, 0)));
        }
        ClearRequest extra{SessionId{"s"}, Epoch{"e"}, OperationId{"extra"}, false, 1, "ticket"};
        QVERIFY(isError(ops.begin("client", extra, 4, 100, 0)));
        ops.cancelSession("client", "s");
        for (const auto& record : ops.records()) QVERIFY(record->cancelled->load());
        ops.sweep(101); QCOMPARE(ops.size(), size_t(32)); // pending execution cannot be forgotten
    }
    void cancellingWaiterClosesParserGateAndLateResultIsRetained() {
        ClearOperations ops;
        ClearRequest request{SessionId{"s"}, Epoch{"e"}, OperationId{"op"}, false, 1, "ticket"};
        auto result = ops.begin("client", request, 4, 100, 0);
        QVERIFY(!isError(result)); auto record = std::get<ClearOperations::Admission>(result).record;
        int callbacks = 0;
        QVERIFY(ops.wait(record, "rpc", [&callbacks](auto) { ++callbacks; }));
        ops.cancel("rpc"); QVERIFY(record->cancelled->load());
        ops.finish(record->id, ClearCompletion{2});
        QCOMPARE(callbacks, 0);
        QVERIFY(ops.wait(record, "retry", [&callbacks](auto replay) {
            QVERIFY(!isError(replay)); QCOMPARE(std::get<ClearCompletion>(replay).generation, quint64(2)); ++callbacks;
        }));
        QCOMPARE(callbacks, 1);
    }
    void completionSurvivesReentrantSweep() {
        ClearOperations ops;
        ClearRequest request{SessionId{"s"}, Epoch{"e"}, OperationId{"op"}, false, 1, "ticket"};
        auto result = ops.begin("client", request, 4, 100, 0);
        QVERIFY(!isError(result)); auto record = std::get<ClearOperations::Admission>(result).record;
        int callbacks = 0;
        QVERIFY(ops.wait(record, "one", [&ops, &callbacks](auto) { ops.sweep(101); ++callbacks; }));
        QVERIFY(ops.wait(record, "two", [&callbacks](auto value) { QVERIFY(!isError(value)); ++callbacks; }));
        ops.finish(record->id, ClearCompletion{2});
        QCOMPARE(callbacks, 2); QCOMPARE(ops.size(), size_t(0));
    }
};
QTEST_GUILESS_MAIN(ClearOperationsTests)
#include "ClearOperationsTests.moc"
