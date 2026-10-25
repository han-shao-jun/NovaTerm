#include "recording/HistoryRecorder.h"
#include <QtTest>

using namespace NovaTerm::Recording;

class HistoryRecorderTests : public QObject {
    Q_OBJECT
private slots:
    void preservesRepeatedAnsiCrLfNullInvalidUtf8() {
        auto budget = std::make_shared<QueueBudget>();
        HistoryRecorder recorder({QUuid::createUuid()}, QUuid::createUuid(), 7, budget);
        const QByteArray bytes = QByteArray::fromHex("1b5b33316d410d0a00ffc328").repeated(3);
        QCOMPARE(recorder.submit(bytes), SubmitResult::Accepted);
        auto batch = recorder.takeBatch();
        QCOMPARE(batch.size(), size_t(1));
        QCOMPARE(batch.front().payload, bytes);
        QCOMPARE(batch.front().connectionGeneration, quint64(7));
        QCOMPARE(batch.front().recordId, quint64(1));
        QCOMPARE(budget->used(), qsizetype(0));
    }
    void tailDropProducesGap() {
        auto budget = std::make_shared<QueueBudget>(4096);
        HistoryRecorder recorder({QUuid::createUuid()}, QUuid::createUuid(), 1, budget, 1024);
        QCOMPARE(recorder.submit(QByteArray(500, 'a')), SubmitResult::Accepted);
        QCOMPARE(recorder.submit(QByteArray(2000, 'b')), SubmitResult::GapRecorded);
        recorder.stop("user_stop");
        const auto batch = recorder.takeBatch();
        QCOMPARE(batch.size(), size_t(2));
        QCOMPARE(batch.back().kind, RecordKind::Gap);
        QCOMPARE(batch.back().lostByteCount.value(), quint64(2000));
        QCOMPARE(batch.back().lostRecordCount.value(), quint64(1));
        QCOMPARE(recorder.submit("late"), SubmitResult::Disabled);
    }
    void omitsFileTransferWithBoundary() {
        auto budget = std::make_shared<QueueBudget>();
        HistoryRecorder recorder({QUuid::createUuid()}, QUuid::createUuid(), 1, budget);
        recorder.submit("before");
        recorder.omit(1024, "file_transfer");
        recorder.submit("after");
        const auto batch = recorder.takeBatch();
        QCOMPARE(batch.size(), size_t(3));
        QCOMPARE(batch[1].kind, RecordKind::Gap);
        QCOMPARE(batch[1].reason, QString("file_transfer"));
        QVERIFY(batch[1].payload.isEmpty());
        QCOMPARE(batch[0].payload + batch[2].payload, QByteArray("beforeafter"));
    }
    void globalQueueDoesNotStarveOtherSession() {
        auto budget = std::make_shared<QueueBudget>(4096);
        HistoryRecorder a({QUuid::createUuid()}, QUuid::createUuid(), 1, budget, 1024);
        HistoryRecorder b({QUuid::createUuid()}, QUuid::createUuid(), 1, budget, 1024);
        for (int i = 0; i < 100; ++i) a.submit(QByteArray(500, 'x'));
        QCOMPARE(b.submit("other"), SubmitResult::Accepted);
        QVERIFY(budget->used() <= 4096);
        a.takeBatch(); b.takeBatch();
        QCOMPARE(budget->used(), qsizetype(0));
    }
    void saturatedGlobalQueueReservesRoomForNewSession() {
        auto budget = std::make_shared<QueueBudget>(); std::vector<std::unique_ptr<HistoryRecorder>> busy;
        for (int n = 0; n < 4; ++n) {
            busy.push_back(std::make_unique<HistoryRecorder>(HistoryCaptureId{QUuid::createUuid()}, QUuid::createUuid(), 1, budget));
            for (int i = 0; i < 256; ++i) busy.back()->submit(QByteArray(64 * 1024, 'x'));
        }
        HistoryRecorder newcomer({QUuid::createUuid()}, QUuid::createUuid(), 1, budget);
        QCOMPARE(newcomer.submit(QByteArray(64 * 1024, 'y')), SubmitResult::Accepted);
        QVERIFY(budget->used() <= 64 * 1024 * 1024);
    }
};
QTEST_GUILESS_MAIN(HistoryRecorderTests)
#include "HistoryRecorderTests.moc"
