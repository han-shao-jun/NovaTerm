#include "history/HistorySearchService.h"
#include <QTest>
#include <QTimer>
using namespace NovaTerm::Recording;
class SearchSource : public HistorySourceReader {
public:
    CaptureDescriptor d;
    std::vector<HistoryRecord> records;
    int reads{0};
    SearchSource(){d.id={QUuid::createUuid()};d.integrity=Integrity::UnknownTail;}
    void append(QByteArray bytes,RecordKind kind=RecordKind::Payload){HistoryRecord r;r.captureId=d.id;r.recordId=r.lastRecordId=records.size()+1;r.kind=kind;r.payload=bytes;records.push_back(r);d.highWaterRecordId=records.size();}
    std::vector<CaptureDescriptor> captures() const override{return {d};}
    bool read(HistoryCaptureId,HistoryRange r,std::function<void(ReadResult)> done) override {
        ++reads; ReadResult result;result.highWaterRecordId=d.highWaterRecordId;result.integrity=d.integrity;
        if(r.firstRecordId && r.firstRecordId<=records.size())result.records.push_back(records[size_t(r.firstRecordId-1)]);
        QTimer::singleShot(0,[done,result]{done(result);});return true;
    }
};
class HistorySearchServiceTests : public QObject {
    Q_OBJECT
private slots:
    void shortCompletedSearchesDoNotConsumeCursorSlots(){
        SearchSource source;source.append("hello\n");HistoryAuthorizationStore auth;QVERIFY(auth.share({"c","t",source.d.id}));
        HistoryReadService reads(source,auth,[](const auto&){return true;});HistorySearchService search(reads);
        for(int i=0;i<4;++i){bool done=false;HistorySearchPage p;search.searchAsync({source.d.id,SearchMode::Text,"hello"},{"c","t"},[&](auto v){p=v;done=true;});QTRY_VERIFY(done);
            QVERIFY2(p.error.isEmpty(),qPrintable(p.error));QVERIFY(p.complete);QVERIFY(p.cursor.isEmpty());}
    }
    void rawSpanBudgetSurvivesReadBatches(){
        SearchSource source;for(int i=0;i<512;++i)source.append("a");HistoryAuthorizationStore auth;QVERIFY(auth.share({"c","t",source.d.id}));
        HistoryReadService reads(source,auth,[](const auto&){return true;});HistorySearchService::Limits limits;limits.pageTime=std::chrono::seconds(15);HistorySearchService search(reads,limits);bool done=false;HistorySearchPage p;
        search.searchAsync({source.d.id,SearchMode::RawBytes,QByteArray(128,'a'),{},8},{"c","t"},[&](auto v){p=v;done=true;});QTRY_VERIFY_WITH_TIMEOUT(done,20000);
        size_t spans=0;for(const auto& match:p.matches)spans+=match.spans.size();QVERIFY(spans<=128);QCOMPARE(p.matches.size(),size_t(1));QVERIFY(!p.complete);QVERIFY(!p.cursor.isEmpty());
    }
    void eofSpanBudgetKeepsDeferredHit(){
        SearchSource source;for(int i=0;i<130;++i)source.append("a");HistoryAuthorizationStore auth;QVERIFY(auth.share({"c","t",source.d.id}));
        HistoryReadService reads(source,auth,[](const auto&){return true;});HistorySearchService search(reads);bool done=false;HistorySearchPage p;
        HistorySearchRequest r{source.d.id,SearchMode::Text,QByteArray(65,'a'),{},8};search.searchAsync(r,{"c","t"},[&](auto v){p=v;done=true;});QTRY_VERIFY(done);
        QCOMPARE(p.matches.size(),size_t(1));QVERIFY(!p.complete);QVERIFY(!p.cursor.isEmpty());r.cursor=p.cursor;done=false;
        search.searchAsync(r,{"c","t"},[&](auto v){p=v;done=true;});QTRY_VERIFY(done);QCOMPARE(p.matches.size(),size_t(1));QVERIFY(p.complete);
    }
    void textMatchCanEndImmediatelyBeforeGap(){
        SearchSource source;source.append("a\n");source.append("b");source.append({},RecordKind::Gap);
        HistoryAuthorizationStore auth;QVERIFY(auth.share({"c","t",source.d.id}));HistoryReadService reads(source,auth,[](const auto&){return true;});HistorySearchService search(reads);
        bool done=false;HistorySearchPage p;search.searchAsync({source.d.id,SearchMode::Text,"a\nb"},{"c","t"},[&](auto v){p=v;done=true;});QTRY_VERIFY(done);
        QCOMPARE(p.matches.size(),size_t(1));QVERIFY(p.complete);
    }
    void pagedMatchesEqualContinuousScanAndRetryDoesNotAdvance(){
        SearchSource source;source.append("he");source.append("llo hello hello");
        HistoryAuthorizationStore auth;QVERIFY(auth.share({"c","t",source.d.id}));
        HistoryReadService reads(source,auth,[](const auto&){return true;});HistorySearchService search(reads);
        HistorySearchRequest request{source.d.id,SearchMode::RawBytes,"hello",{},1};
        HistorySearchPage p;bool done=false;search.searchAsync(request,{"c","t"},[&](auto v){p=v;done=true;});QTRY_VERIFY(done);
        QVERIFY(p.error.isEmpty());QCOMPARE(p.matches.size(),size_t(1));QVERIFY(!p.complete);QVERIFY(!p.cursor.isEmpty());
        request.cursor=p.cursor;done=false;search.searchAsync(request,{"c","t"},[&](auto v){p=v;done=true;});QTRY_VERIFY(done);
        const auto next=p.cursor;const auto count=source.reads;done=false;
        search.searchAsync(request,{"c","t"},[&](auto v){p=v;done=true;});QTRY_VERIFY(done);
        QCOMPARE(p.cursor,next);QCOMPARE(source.reads,count);QCOMPARE(p.matches.size(),size_t(1));
        request.query="changed";done=false;search.searchAsync(request,{"c","t"},[&](auto v){p=v;done=true;});QTRY_VERIFY(done);
        QCOMPARE(p.error,QString("CURSOR_MISMATCH"));
    }
    void budgetInsideUtf8AnsiAndLiteral(){
        SearchSource source;source.append(QByteArray("\x1b[31m")+QString::fromUtf8("中文").toUtf8()+"\x1b[0m\n");
        HistoryAuthorizationStore auth;QVERIFY(auth.share({"c","t",source.d.id}));
        HistoryReadService reads(source,auth,[](const auto&){return true;});HistorySearchService::Limits limits;limits.scanBytes=1;
        HistorySearchService search(reads,limits);HistorySearchRequest r{source.d.id,SearchMode::Text,QString::fromUtf8("中文").toUtf8()};
        size_t matches=0;int pages=0;HistorySearchPage p;
        do{bool done=false;search.searchAsync(r,{"c","t"},[&](auto v){p=v;done=true;});QTRY_VERIFY(done);QVERIFY2(p.error.isEmpty(),qPrintable(p.error));
            matches+=p.matches.size();r.cursor=p.cursor;QVERIFY(++pages<100);
        }while(!r.cursor.isEmpty());
        QVERIFY(p.complete);QCOMPARE(matches,size_t(1));QCOMPARE(p.integrity,Integrity::UnknownTail);
    }
    void cancelledAndRevokedSearchDoesNotDeliverBody(){
        SearchSource source;source.append("hello\n");HistoryAuthorizationStore auth;QVERIFY(auth.share({"c","t",source.d.id}));
        HistoryReadService reads(source,auth,[](const auto&){return true;});HistorySearchService search(reads);bool done=false;HistorySearchPage p;
        auto cancel=std::make_shared<std::atomic<bool>>(false);
        search.searchAsync({source.d.id,SearchMode::Text,"hello"},{"c","t"},[&](auto v){p=v;done=true;},cancel);
        cancel->store(true);auth.revoke("c",source.d.id);QTRY_VERIFY(done);QCOMPARE(p.error,QString("PERMISSION_DENIED"));QVERIFY(p.matches.empty());
    }
    void cursorExpiryDoesNotRestartScan(){
        SearchSource source;source.append("hello hello");HistoryAuthorizationStore auth;QVERIFY(auth.share({"c","t",source.d.id}));
        HistoryReadService reads(source,auth,[](const auto&){return true;});HistorySearchService::Limits limits;limits.idleTtl=std::chrono::milliseconds(5);
        HistorySearchService search(reads,limits);bool done=false;HistorySearchPage p;HistorySearchRequest r{source.d.id,SearchMode::RawBytes,"hello",{},1};
        search.searchAsync(r,{"c","t"},[&](auto v){p=v;done=true;});QTRY_VERIFY(done);r.cursor=p.cursor;const auto count=source.reads;QTest::qWait(10);done=false;
        search.searchAsync(r,{"c","t"},[&](auto v){p=v;done=true;});QTRY_VERIFY(done);QCOMPARE(p.error,QString("SEARCH_CURSOR_EXPIRED"));QCOMPARE(source.reads,count);
    }
};
QTEST_GUILESS_MAIN(HistorySearchServiceTests)
#include "HistorySearchServiceTests.moc"
