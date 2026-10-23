#include "history/HistoryReadService.h"
#include <QTest>
#include <QTimer>
using namespace NovaTerm::Recording;
class FakeHistory : public HistorySourceReader {
public:
    CaptureDescriptor d;
    std::vector<HistoryRecord> records;
    FakeHistory() { d.id = {QUuid::createUuid()}; d.highWaterRecordId = 2; d.integrity = Integrity::Lossless;
        for (int i=1; i<=2; ++i) { HistoryRecord r; r.captureId=d.id; r.recordId=r.lastRecordId=i; r.payload=QByteArray(65536, char('a'+i)); records.push_back(r); } }
    std::vector<CaptureDescriptor> captures() const override { return {d}; }
    bool read(HistoryCaptureId, HistoryRange range, std::function<void(ReadResult)> done) override {
        ReadResult r; r.highWaterRecordId=d.highWaterRecordId; r.integrity=d.integrity;
        for (auto record : records) if (record.recordId>=range.firstRecordId && record.recordId<=range.lastRecordId) { r.records.push_back(record); break; }
        QTimer::singleShot(0, [done, r] { done(r); }); return true;
    }
};
class HistoryReadTests : public QObject {
    Q_OBJECT
private slots:
    void binaryPagesKeepFixedHighWater() {
        FakeHistory source; HistoryAuthorizationStore auth; QVERIFY(auth.share({"c","token",source.d.id}));
        HistoryReadService service(source, auth, [](const auto&){return true;});
        QByteArray collected; QString cursor; quint64 high=0;
        do { bool done=false; HistoryReadPage page;
            service.readAsync({source.d.id,cursor,8192}, {"c","token"}, [&](auto p){page=std::move(p); done=true;});
            QTRY_VERIFY(done); QVERIFY2(page.error.isEmpty(),qPrintable(page.error));
            for (const auto& f:page.records) collected+=f.record.payload;
            cursor=page.cursor; high=page.highWater;
            source.d.highWaterRecordId=3;
        } while (!cursor.isEmpty());
        QCOMPARE(high,2ull); QCOMPARE(collected,source.records[0].payload+source.records[1].payload);
    }
    void revocationDuringReadReturnsNoBody() {
        FakeHistory source; HistoryAuthorizationStore auth; QVERIFY(auth.share({"c","token",source.d.id}));
        HistoryReadService service(source,auth,[](const auto&){return true;}); bool done=false; HistoryReadPage page;
        service.readAsync({source.d.id}, {"c","token"}, [&](auto p){page=std::move(p);done=true;});
        auth.revoke("c",source.d.id); QTRY_VERIFY(done);
        QCOMPARE(page.error,QString("PERMISSION_DENIED")); QVERIFY(page.records.empty());
    }
    void rangeShrinkRejectsOldCursor() {
        FakeHistory source; HistoryAuthorizationStore auth; QVERIFY(auth.share({"c","token",source.d.id}));
        HistoryReadService service(source,auth,[](const auto&){return true;}); bool done=false; HistoryReadPage page;
        service.readAsync({source.d.id,{},1},{"c","token"},[&](auto p){page=std::move(p);done=true;}); QTRY_VERIFY(done);
        QVERIFY(!page.cursor.isEmpty()); QVERIFY(auth.share({"c","token",source.d.id,2,2})); done=false;
        service.readAsync({source.d.id,page.cursor,1},{"c","token"},[&](auto p){page=std::move(p);done=true;});
        QTRY_VERIFY(done); QCOMPARE(page.error,QString("PERMISSION_DENIED")); QVERIFY(page.records.empty());
    }
};
QTEST_GUILESS_MAIN(HistoryReadTests)
#include "HistoryReadTests.moc"
