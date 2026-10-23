#include "evidence/EvidenceStore.h"
#include "evidence/ImportedResourceStore.h"
#include <QTest>
using namespace NovaTerm::Evidence;
class FakeEvidenceSource : public EvidenceSourceReader {
public:
    QByteArray bytes{"bytes"};bool allowed{true};
    bool authorized(const Source&,const Context& c) const override{return allowed&&c.principal=="owner";}
    void read(const Source&,const Context&,Done done) override{done({bytes,{},"unknown_tail",{{"route","shared_terminal"}}});}
};
class EvidenceTests : public QObject {
    Q_OBJECT
private slots:
    void explicitImportedResourceDeletionExpiresEvidence(){
        auto source=std::make_shared<ImportedResourceStore>();EvidenceStore store;QVERIFY(store.registerSource(SourceKind::ImportedFile,source));
        const auto resource=source->publish("a binary\0payload",{"owner","credential"});QVERIFY(resource);
        Result result;store.create(*resource,{"owner","credential"},0,[&](auto r){result=std::move(r);});QVERIFY(result.reference);const auto id=result.reference->id;
        source->remove(resource->resource);store.resolve(id,{"owner","credential"},[&](auto r){result=std::move(r);});QCOMPARE(result.error,QString("EVIDENCE_EXPIRED"));QVERIFY(result.bytes.isEmpty());
        // Model-shaped filesystem paths have no resolution capability.
        source->read({SourceKind::ImportedFile,"C:/private/log.txt",1,{{0,0,1}}},{"owner","credential"},[&](auto r){QVERIFY(!r.error.isEmpty());QVERIFY(r.bytes.isEmpty());});
    }
    void allThreeSourcesRecheckOwnerHashAndExpiry(){
        for(auto kind:{SourceKind::TerminalCapture,SourceKind::DiagnosticResult,SourceKind::ImportedFile}){
            EvidenceStore store;auto source=std::make_shared<FakeEvidenceSource>();QVERIFY(store.registerSource(kind,source));
            Result r;store.create({kind,"resource",1,{{1,0,5}}},{"owner","token"},1,[&](auto result){r=result;});
            QVERIFY(r.reference);const auto id=r.reference->id;QCOMPARE(r.reference->sourceCompleteness,QString("unknown_tail"));QVERIFY(!r.reference->persistent);QVERIFY(r.reference->expiresAtMs>r.reference->collectedMs);
            store.resolve(id,{"other","token"},[&](auto result){r=result;});QCOMPARE(r.error,QString("PERMISSION_DENIED"));QVERIFY(r.bytes.isEmpty());
            source->bytes="changed";store.resolve(id,{"owner","token"},[&](auto result){r=result;});QCOMPARE(r.error,QString("EVIDENCE_EXPIRED"));
            source->allowed=false;store.resolve(id,{"owner","token"},[&](auto result){r=result;});QCOMPARE(r.error,QString("PERMISSION_DENIED"));
            EvidenceStore restarted;QVERIFY(restarted.registerSource(kind,source));restarted.resolve(id,{"owner","token"},[&](auto result){r=result;});QCOMPARE(r.error,QString("EVIDENCE_EXPIRED"));
        }
    }
};
QTEST_GUILESS_MAIN(EvidenceTests)
#include "EvidenceTests.moc"
