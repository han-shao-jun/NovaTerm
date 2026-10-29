#include "ui/widgets/HistoryRecordingPanel.h"
#include "ui/models/HistoryCaptureModel.h"
#include "recording/HistoryService.h"
#include <ElaApplication.h>
#include <ElaPushButton.h>
#include <ElaSpinBox.h>
#include "recording/HistoryChunkStore.h"
#include <thread>
#include <QTableView>
#include <QTemporaryDir>
#include <QtTest>
#include <QFontDatabase>
#include <QLineEdit>
#include <ElaCheckBox.h>
#include <QComboBox>
#include <QLabel>
#include <QFileDialog>
#include <QTimer>

using namespace NovaTerm::Recording;
template<class T> T* control(QWidget& panel, const char* role) {
    for (auto* widget : panel.findChildren<T*>()) if (widget->property("historyCaptureRole").toString() == QLatin1String(role)) return widget;
    return nullptr;
}
class HistoryRecordingUiTests : public QObject {
    Q_OBJECT
private slots:
    void sharingUsesOneAccessForAllAi() {
        QTemporaryDir dir; HistoryService service(dir.path()); HistoryRecordingPanel panel(&service);
        QVERIFY2(panel.findChildren<QComboBox*>().isEmpty(), "History sharing must not ask users to select an AI client");
        bool sharedAccessExplained = false;
        for (auto* label : panel.findChildren<QLabel*>())
            sharedAccessExplained |= label->text().contains("all AI", Qt::CaseInsensitive);
        QVERIFY2(sharedAccessExplained, "History sharing must explain that permission applies to all AI connections");
    }
    void identityChangeDuringFileSelectionRejectsImport_data() {
        QTest::addColumn<QString>("replacementId");
        QTest::addColumn<quint64>("replacementIdentity");
        QTest::newRow("token-reset") << QString("old-access") << quint64(2);
        QTest::newRow("identity-replaced") << QString("new-access") << quint64(1);
        QTest::newRow("disabled") << QString{} << quint64(0);
    }
    void identityChangeDuringFileSelectionRejectsImport() {
        QFETCH(QString, replacementId); QFETCH(quint64, replacementIdentity);
        QTemporaryDir dir; HistoryService service(dir.path()); HistoryRecordingPanel panel(&service);
        const auto path = dir.filePath("shared.log");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("history"); file.close();
        int imported = 0;
        HistorySharingActions initial; initial.principalId = "old-access"; initial.identityVersion = 1;
        initial.importFile = [&](QString, QString, std::function<void(QString)>) { ++imported; };
        panel.setSharingActions(std::move(initial));
        ElaPushButton* importButton = nullptr;
        for (auto* button : panel.findChildren<ElaPushButton*>())
            if (button->text() == "Share local log file…") importButton = button;
        QVERIFY(importButton); QVERIFY(importButton->isEnabled());
        QTimer::singleShot(0, &panel, [&] {
            HistorySharingActions replacement; replacement.principalId = replacementId; replacement.identityVersion = replacementIdentity;
            replacement.importFile = [&](QString, QString, std::function<void(QString)>) { ++imported; };
            panel.setSharingActions(std::move(replacement));
            auto* picker = panel.findChild<QFileDialog*>();
            QVERIFY(picker); picker->selectFile(path);
            QVERIFY(QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection));
        });
        importButton->click();
        QCOMPARE(imported, 0);
    }
    void staleSharingDoesNotChangeLocalRecording() {
        QTemporaryDir dir; HistoryService service(dir.path()); QTRY_VERIFY(service.ready());
        const RecordingIntent intent{QUuid::createUuid(), 1}; bool done = false;
        QVERIFY(service.start(intent, [&](auto r) { QVERIFY(r.success); done = true; })); QTRY_VERIFY(done);
        service.stop(intent); QTRY_VERIFY(!service.captures().front().active);
        HistoryRecordingPanel panel(&service);
        bool current = true; int grants = 0, revocations = 0, clears = 0;
        HistorySharingActions actions; actions.principalId = "shared-access"; actions.identityVersion = 1;
        actions.isCurrent = [&] { return current; };
        actions.share = [&](QString, HistoryCaptureId, quint64, quint64, bool) { ++grants; return QString{}; };
        actions.revoke = [&](QString, HistoryCaptureId) { ++revocations; return QString{}; };
        actions.clearImports = [&](QString) { ++clears; };
        panel.setSharingActions(actions);
        auto* table = control<QTableView>(panel, "historyCaptures"); QVERIFY(table); table->selectRow(0);
        auto* share = control<ElaPushButton>(panel, "historyShare"); QVERIFY(share && share->isEnabled());
        const auto capture = service.captures().front().id;
        current = false; // Event delivery may lag the access store's new identity.
        share->click(); control<ElaPushButton>(panel, "historyUnshare")->click();
        control<ElaPushButton>(panel, "historyClearImports")->click();
        QCOMPARE(grants, 0); QCOMPARE(revocations, 0); QCOMPARE(clears, 0);
        panel.setSharingActions(std::move(actions)); QVERIFY(!share->isEnabled());
        QVERIFY(!control<ElaPushButton>(panel, "historyClearImports")->isEnabled());
        QCOMPARE(service.captures().size(), size_t(1)); QCOMPARE(service.captures().front().id, capture);
        QVERIFY(control<ElaPushButton>(panel, "historyDelete")->isEnabled());
    }
    void offlineSharingFormRejectsInvalidRanges() {
        QTemporaryDir dir; HistoryService service(dir.path()); QTRY_VERIFY(service.ready());
        const RecordingIntent intent{QUuid::createUuid(), 1}; bool done = false;
        QVERIFY(service.start(intent, [&](auto r) { QVERIFY(r.success); done = true; })); QTRY_VERIFY(done);
        service.stop(intent); QTRY_VERIFY(!service.captures().front().active);
        HistoryRecordingPanel panel(&service);
        int granted = 0; HistoryCaptureId chosen; quint64 first = 0, last = 9; bool persistent = false;
        HistorySharingActions actions; actions.principalId = "shared-access"; actions.identityVersion = 1;
        actions.share = [&](QString client, HistoryCaptureId id, quint64 a, quint64 b, bool keep) {
            if (client == "shared-access") { ++granted; chosen = id; first = a; last = b; persistent = keep; } return QString{};
        };
        panel.setSharingActions(std::move(actions));
        auto* table = control<QTableView>(panel, "historyCaptures"); QVERIFY(table); table->selectRow(0);
        auto* from = control<QLineEdit>(panel, "historyShareFirst");
        auto* to = control<QLineEdit>(panel, "historyShareLast");
        auto* share = control<ElaPushButton>(panel, "historyShare");
        auto* keep = control<ElaCheckBox>(panel, "historySharePersistent");
        QVERIFY(from && to && share && keep); QVERIFY(share->isEnabled());
        from->setText("2"); to->setText("1"); share->click(); QCOMPARE(granted, 0);
        from->setText("1"); to->setText("0"); keep->setChecked(true); share->click();
        QCOMPARE(granted, 1); QCOMPARE(chosen, service.captures().front().id);
        QCOMPARE(first, quint64(1)); QCOMPARE(last, quint64(0)); QVERIFY(persistent);
        const auto preview = qEnvironmentVariable("NOVATERM_HISTORY_SHARING_UI_PREVIEW");
        if (!preview.isEmpty()) { panel.resize(1000, 820); panel.show(); QTest::qWait(100); QVERIFY(panel.grab().save(preview)); }
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
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
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
