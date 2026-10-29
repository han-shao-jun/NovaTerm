#include "ui/widgets/RemoteDirectoryDialog.h"
#include <ElaApplication.h>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QLabel>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>
#include <memory>

class RemoteDirectoryDialogTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { eApp->init(); }
    void navigationFiltersFilesAndSelectsVerifiedDirectory() {
        QList<QString> paths;
        RemoteDirectoryDialog::Completion answer;
        RemoteDirectoryDialog dialog(".", [&](const QString& path, QObject*, auto done) {
            paths.append(path); answer = std::move(done);
        }, [] { return true; });
        QTRY_COMPARE(paths.size(), 1); QCOMPARE(paths.back(), QString("."));
        auto* tree = dialog.findChild<QTreeWidget*>("remoteDirectoryTree");
        auto* select = dialog.findChild<QPushButton*>("remoteDirectorySelect");
        QVERIFY(tree && select); QVERIFY(!select->isEnabled());
        SftpFileInfo directory; directory.name = "build"; directory.path = "/home/test/build"; directory.directory = true;
        SftpFileInfo file; file.name = "file"; file.path = "/home/test/file";
        answer({"/home/test", {directory, file}, {}, false});
        QCOMPARE(tree->topLevelItemCount(), 1); QVERIFY(select->isEnabled());
        auto* row = tree->topLevelItem(0);
        tree->itemDoubleClicked(row, 0);
        tree->itemActivated(row, 0);
        QTRY_COMPARE(paths.size(), 2);
        QCOMPARE(paths.back(), QString("/home/test/build")); QVERIFY(!select->isEnabled());
        answer({"/home/test/build", {}, {}, false});
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        select->click(); QCOMPARE(accepted.size(), 1);
        QCOMPARE(dialog.selectedDirectory(), QString("/home/test/build"));
    }
    void staleResponsesAndDestroyedContextsAreDiscarded() {
        QList<RemoteDirectoryDialog::Completion> answers;
        QList<QPointer<QObject>> contexts;
        auto dialog = std::make_unique<RemoteDirectoryDialog>("/one", [&](const QString&, QObject* context, auto done) {
            contexts.append(context); answers.append(std::move(done));
        }, [] { return true; });
        QTRY_COMPARE(answers.size(), 1);
        answers.first()({"/one", {}, {}, false});
        dialog->findChild<QLineEdit*>("remoteDirectoryPath")->setText("/two");
        dialog->findChild<QPushButton*>("remoteDirectoryGo")->click();
        QCOMPARE(answers.size(), 2); QVERIFY(contexts.first().isNull());
        answers.first()({"/one", {}, {}, false});
        QVERIFY(!dialog->findChild<QPushButton*>("remoteDirectorySelect")->isEnabled());
        answers.last()({"/two", {}, {}, false});
        QVERIFY(dialog->findChild<QPushButton*>("remoteDirectorySelect")->isEnabled());
        dialog.reset(); QVERIFY(contexts.last().isNull());
        answers.last()({"/two", {}, {}, false});
    }
    void invalidatedIdentityAndUnverifiedEditsCannotBeAccepted() {
        bool current = true;
        RemoteDirectoryDialog::Completion answer;
        RemoteDirectoryDialog dialog("/one", [&](const QString&, QObject*, auto done) { answer = std::move(done); }, [&] { return current; });
        QTRY_VERIFY(bool(answer)); answer({"/one", {}, {}, false});
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        dialog.findChild<QLineEdit*>("remoteDirectoryPath")->setText("/unverified");
        static_cast<QDialog*>(&dialog)->accept(); QCOMPARE(accepted.size(), 0);
        dialog.findChild<QLineEdit*>("remoteDirectoryPath")->setText("/one");
        current = false;
        static_cast<QDialog*>(&dialog)->accept(); QCOMPARE(accepted.size(), 0);
    }
    void errorsCanBeRetriedAndTruncationIsVisible() {
        RemoteDirectoryDialog::Completion answer;
        RemoteDirectoryDialog dialog("/one", [&](const QString&, QObject*, auto done) { answer = std::move(done); }, [] { return true; });
        QTRY_VERIFY(bool(answer)); answer({{}, {}, "permission denied", false});
        QVERIFY(!dialog.findChild<QPushButton*>("remoteDirectorySelect")->isEnabled());
        auto* status = dialog.findChild<QLabel*>("remoteDirectoryStatus"); QVERIFY(status);
        QCOMPARE(status->text(), QString("permission denied"));
        dialog.findChild<QPushButton*>("remoteDirectoryGo")->click();
        answer({"/one", {}, {}, true});
        QVERIFY(!status->text().isEmpty()); QVERIFY(dialog.findChild<QPushButton*>("remoteDirectorySelect")->isEnabled());
    }
    void cancellationDiscardsRequestAndRootCannotBeGranted() {
        QPointer<QObject> context;
        RemoteDirectoryDialog::Completion answer;
        RemoteDirectoryDialog dialog("/", [&](const QString&, QObject* receiver, auto done) { context = receiver; answer = std::move(done); }, [] { return true; });
        QTRY_VERIFY(bool(answer)); answer({"/", {}, {}, false});
        QVERIFY(!dialog.findChild<QPushButton*>("remoteDirectorySelect")->isEnabled());
        dialog.reject(); QVERIFY(context.isNull());
        answer({"/late", {}, {}, false}); QVERIFY(dialog.selectedDirectory().isEmpty());
    }
    void overlongUtf8PathIsNotTruncatedOrGranted_data() {
        QTest::addColumn<QString>("path");
        QTest::newRow("ascii") << QString("/") + (QString(149, 'd') + "/").repeated(8) + "leaf";
        QTest::newRow("utf8") << QString("/") + (QString(50, QChar(0x4e2d)) + "/").repeated(8) + "leaf";
    }
    void overlongUtf8PathIsNotTruncatedOrGranted() {
        QFETCH(QString, path);
        RemoteDirectoryDialog::Completion answer;
        RemoteDirectoryDialog dialog(".", [&](const QString&, QObject*, auto done) { answer = std::move(done); }, [] { return true; });
        QTRY_VERIFY(bool(answer)); answer({path, {}, {}, false});
        QCOMPARE(dialog.findChild<QLineEdit*>("remoteDirectoryPath")->text(), path);
        auto* select = dialog.findChild<QPushButton*>("remoteDirectorySelect"); QVERIFY(!select->isEnabled());
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        dialog.accept(); QCOMPARE(accepted.size(), 0);
        QVERIFY(!dialog.findChild<QLabel*>("remoteDirectoryStatus")->text().isEmpty());
        dialog.findChild<QLineEdit*>("remoteDirectoryPath")->setText("/short");
        dialog.findChild<QPushButton*>("remoteDirectoryGo")->click();
        answer({"/short", {}, {}, false}); QVERIFY(select->isEnabled());
        select->click(); QCOMPARE(dialog.selectedDirectory(), QString("/short"));
    }
};
QTEST_MAIN(RemoteDirectoryDialogTests)
#include "RemoteDirectoryDialogTests.moc"
