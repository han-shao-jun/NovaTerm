#include "ui/widgets/HistoryRecordingPanel.h"
#include "ui/models/HistoryCaptureModel.h"
#include "history/HistoryService.h"
#include <ElaApplication.h>
#include <ElaPushButton.h>
#include <ElaSpinBox.h>
#include "history/HistoryChunkStore.h"
#include <thread>
#include <QTableView>
#include <QTemporaryDir>
#include <QtTest>
#include <QFontDatabase>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTimer>
#include <QComboBox>

using namespace NovaTerm::Recording;
template<class T> T* control(QWidget& panel, const char* role) {
    for (auto* widget : panel.findChildren<T*>()) if (widget->property("historyCaptureRole").toString() == QLatin1String(role)) return widget;
    return nullptr;
}
class HistoryRecordingUiTests : public QObject {
    Q_OBJECT
private slots:
    void offlineSharingRequiresExplicitClientAndRange() {
        struct Sharing final:HistorySharingPort {
            std::optional<ResourceGrant> saved;
            std::vector<SharingClient> clients()const override{return {{"client-a","Client A"},{"client-b","Client B"}};}
            std::optional<ResourceGrant> grant(const QString&,HistoryCaptureId)const override{return {};}
            bool share(ResourceGrant g)override{saved=std::move(g);return true;}
            bool revoke(const QString&,HistoryCaptureId)override{return true;}
        } sharing;
        QTemporaryDir dir;HistoryService service(dir.path());HistoryRecordingPanel panel(&service);
        const RecordingIntent intent{QUuid::createUuid(),1};bool started=false;
        QTRY_VERIFY(service.ready());QVERIFY(service.start(intent,[&](auto r){started=r.success;}));QTRY_VERIFY(started);
        service.submit(intent.sessionId,1,"recorded");service.stop(intent);QTRY_VERIFY(!service.captures().front().active);
        auto* table=control<QTableView>(panel,"historyCaptures");QTRY_COMPARE(table->model()->rowCount(),1);table->selectRow(0);
        auto* share=control<ElaPushButton>(panel,"historyShare");QVERIFY(share);QVERIFY(!share->isEnabled());
        panel.setSharingPort(&sharing);QVERIFY(share->isEnabled());QVERIFY(!sharing.saved);
        QTimer::singleShot(0,&panel,[&]{
            auto* dialog=panel.findChild<QDialog*>();QVERIFY(dialog);auto* client=dialog->findChild<QComboBox*>();QVERIFY(client);client->setCurrentIndex(1);
            const auto edits=dialog->findChildren<QLineEdit*>();QCOMPARE(edits.size(),4);edits[0]->setText("1");edits[1]->setText("1");
            auto* buttons=dialog->findChild<QDialogButtonBox*>();QVERIFY(buttons);buttons->button(QDialogButtonBox::Save)->click();
        });
        share->click();QVERIFY(sharing.saved);QCOMPARE(sharing.saved->principal,QString("client-b"));QCOMPARE(sharing.saved->capture,service.captures().front().id);
        QCOMPARE(sharing.saved->first,1ull);QCOMPARE(sharing.saved->last,1ull);QVERIFY(!sharing.saved->persistent);
    }
    void partialSpinboxTextSurvivesExternalRefresh() {
        QTemporaryDir dir; HistoryService service(dir.path()); HistoryRecordingPanel panel(&service);
        const auto spins = panel.findChildren<ElaSpinBox*>(); QTRY_VERIFY(spins[0]->isEnabled());
        auto* edit = spins[0]->findChild<QLineEdit*>(); QVERIFY(edit);
        QSignalSpy valueChanges(spins[0], &QSpinBox::valueChanged);
        spins[0]->selectAll(); QTest::keyClick(spins[0], Qt::Key_Backspace);
        QCOMPARE(valueChanges.size(), 0); QVERIFY(!spins[0]->hasAcceptableInput()); const auto pending = edit->text();
        bool configured = false;
        QVERIFY(service.configure({64 * 1024 * 1024, 8 * 1024 * 1024, 259200000}, [&](auto r) { QVERIFY(r.success); configured = true; })); QTRY_VERIFY(configured);
        QCOMPARE(edit->text(), pending);
    }
    void externalSettingsDoNotOverwritePendingEdits() {
        QTemporaryDir dir; HistoryService service(dir.path()); HistoryRecordingPanel panel(&service);
        const auto spins = panel.findChildren<ElaSpinBox*>(); QTRY_VERIFY(spins[0]->isEnabled());
        spins[0]->setValue(24); spins[1]->setValue(12); bool configured = false;
        QVERIFY(service.configure({64 * 1024 * 1024, 8 * 1024 * 1024, 259200000}, [&](auto r) { QVERIFY(r.success); configured = true; })); QTRY_VERIFY(configured);
        QCOMPARE(spins[0]->value(), 24); QCOMPARE(spins[1]->value(), 12);
        for (auto* button : panel.findChildren<ElaPushButton*>()) {
            if (button->text() == "Apply retention limits") { button->click(); break; }
        }
        QVERIFY(!spins[0]->isEnabled()); QTRY_VERIFY(spins[0]->isEnabled());
        QCOMPARE(service.settings().globalBytes, quint64(24 * 1024 * 1024));
        QCOMPARE(service.settings().captureBytes, quint64(12 * 1024 * 1024));
    }
    void startRevalidatesConnectionGeneration() {
        QTemporaryDir dir; HistoryService service(dir.path()); HistoryRecordingPanel panel(&service);
        const auto id = QUuid::createUuid(); panel.setRecordingTarget({id, 1}, true, "Fake terminal");
        panel.setTargetResolver([id] { return RecordingIntent{id, 2}; });
        auto* start = control<ElaPushButton>(panel, "historyStart"); QTRY_VERIFY(start->isEnabled()); start->click();
        QTRY_VERIFY(!service.captures().empty()); QCOMPARE(service.captures().front().connectionGeneration, quint64(2));
    }
    void asyncSettingsLoadDoesNotShowWritableDefaults() {
        QTemporaryDir dir; CaptureDescriptor d; d.id = {QUuid::createUuid()};
        { HistoryChunkStore s(dir.path(), d); QVERIFY(s.open()); HistoryRecord r; r.recordId = 1; r.payload = "raw";
          QVERIFY(s.append({r})); QVERIFY(s.commit()); QVERIFY(s.seal("stop")); }
        { QFile settings(dir.path()+"/retention.json"); QVERIFY(settings.open(QIODevice::WriteOnly));
          settings.write("{\"global\":\"16777216\",\"capture\":\"8388608\",\"ttl\":\"259200000\"}"); }
        bool delayed = false; HistoryService service(dir.path(), nullptr, [&delayed](IoPoint p) {
            if (p == IoPoint::RecoveryScan && !delayed) { delayed = true; std::this_thread::sleep_for(std::chrono::milliseconds(700)); } return true;
        });
        HistoryRecordingPanel panel(&service); const auto spins = panel.findChildren<ElaSpinBox*>(); QCOMPARE(spins.size(), 3);
        const bool initiallyDisabled = !spins[0]->isEnabled(); QTest::qWait(900);
        QVERIFY(initiallyDisabled); QTRY_VERIFY(spins[0]->isEnabled());
        QCOMPARE(spins[0]->value(), 16); QCOMPARE(spins[1]->value(), 8); QCOMPARE(spins[2]->value(), 3);
    }
    void initTestCase() {
        eApp->init();
#ifdef Q_OS_WIN
        // offscreen has no Windows font engine discovery; load a real system font.
        QVERIFY(QFontDatabase::addApplicationFont("C:/Windows/Fonts/segoeui.ttf") >= 0);
        QApplication::setFont(QFont("Segoe UI", 10));
#endif
    }
    void localIntentAndOfflineIntegrity() {
        QTemporaryDir dir; HistoryService service(dir.path()); HistoryRecordingPanel panel(&service);
        auto* start = control<ElaPushButton>(panel, "historyStart");
        auto* stop = control<ElaPushButton>(panel, "historyStop");
        auto* table = control<QTableView>(panel, "historyCaptures");
        QVERIFY(start); QVERIFY(stop); QVERIFY(table); QVERIFY(!start->isEnabled());
        const auto id = QUuid::createUuid(); panel.setRecordingTarget({id, 1}, true, "Fake terminal"); QTRY_VERIFY(start->isEnabled());
        start->click(); QTRY_COMPARE(table->model()->rowCount(), 1);
        QTRY_VERIFY(!start->isEnabled()); QCOMPARE(service.submit(id, 1, "raw"), SubmitResult::Accepted);
        table->selectRow(0); QTRY_VERIFY(stop->isEnabled()); stop->click();
        QTRY_VERIFY(!service.captures().front().active);
        QVERIFY(table->model()->index(0, 2).data().toString().contains("complete"));
        panel.setRecordingTarget({}, false, {}); QVERIFY(!start->isEnabled());
        QVERIFY(table->model()->rowCount() == 1); // Offline captures remain visible.
        const auto preview = qEnvironmentVariable("NOVATERM_HISTORY_UI_PREVIEW");
        if (!preview.isEmpty()) { panel.show(); QTest::qWait(100); QVERIFY(panel.grab().save(preview)); }
    }
    void liveUpdatesPreserveSelectedCapture() {
        QTemporaryDir dir; HistoryService service(dir.path()); HistoryRecordingPanel panel(&service);
        const auto id = QUuid::createUuid(); panel.setRecordingTarget({id, 1}, true, "Fake terminal");
        QTRY_VERIFY(control<ElaPushButton>(panel, "historyStart")->isEnabled());
        control<ElaPushButton>(panel, "historyStart")->click();
        auto* table = control<QTableView>(panel, "historyCaptures"); QTRY_COMPARE(table->model()->rowCount(), 1);
        table->selectRow(0); service.submit(id, 1, "update");
        QTRY_VERIFY(service.captures().front().highWaterRecordId > 0);
        QCOMPARE(table->currentIndex().row(), 0);
    }
};
QTEST_MAIN(HistoryRecordingUiTests)
#include "HistoryRecordingUiTests.moc"
