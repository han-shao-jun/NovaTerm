#include "history/HistoryProjectionDecoder.h"
#include <QTest>
using namespace NovaTerm::Recording;
class HistorySearchTests : public QObject {
    Q_OBJECT
private slots:
    void literalAcrossChunk() {
        HistoryLiteralMatcher matcher("hello"); std::optional<HistoryMatch> hit;
        int offset=0; for (char c:QByteArray("he")) hit=matcher.feed(c,{1,quint32(offset++),1});
        QVERIFY(!hit); offset=0; for (char c:QByteArray("llo")) hit=matcher.feed(c,{2,quint32(offset++),1});
        QVERIFY(hit); QCOMPARE(hit->spans.size(),size_t(2)); QCOMPARE(hit->spans[0].length,2u); QCOMPARE(hit->spans[1].length,3u);
    }
    void splitUtf8AndAnsi() {
        HistoryProjectionDecoder decoder; QByteArray text; std::vector<RawSpan> spans;
        const auto output=[&](auto unit) {text+=unit.bytes; for (auto s:unit.spans) appendSpan(spans,s);};
        const auto raw=QString::fromUtf8("A\x1b[31m中\x1b[0mB\n").toUtf8();
        for (qsizetype i=0;i<raw.size();++i) QVERIFY(decoder.feed(raw[i],{quint64(i+1),0,1},output));
        QCOMPARE(text,QString::fromUtf8("A中B\n").toUtf8()); QCOMPARE(spans.size(),size_t(6));
    }
    void crProjectionMapsToRaw() {
        HistoryProjectionDecoder d; QByteArray text; std::vector<RawSpan> spans;
        const auto output=[&](auto u){text+=u.bytes; for(auto s:u.spans) appendSpan(spans,s);};
        QByteArray raw("abc\rX\n"); for (qsizetype i=0;i<raw.size();++i) QVERIFY(d.feed(raw[i],{1,quint32(i),1},output));
        QCOMPARE(text,QByteArray("Xbc\n")); QCOMPARE(spans[0].offset,4u); QCOMPARE(spans[1].offset,1u);
    }
    void matchDoesNotCrossGapOrBoot() {
        HistoryLiteralMatcher matcher("abc"); matcher.feed('a',{1,0,1}); matcher.reset();
        QVERIFY(!matcher.feed('b',{3,0,1})); QVERIFY(!matcher.feed('c',{3,1,1}));
    }
    void longLineProjectionLimitIsExplicit() {
        HistoryProjectionDecoder d; bool ok=true; int i=0;
        for (;i<70000 && ok;++i) ok=d.feed('x',{1,quint32(i),1},[](auto){});
        QVERIFY(!ok); QVERIFY(i<=65537); QVERIFY(!d.limitRange().empty());
    }
};
QTEST_GUILESS_MAIN(HistorySearchTests)
#include "HistorySearchTests.moc"
