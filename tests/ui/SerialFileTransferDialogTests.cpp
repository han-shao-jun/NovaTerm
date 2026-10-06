/** @file SerialFileTransferDialogTests.cpp @brief 串口传输窗口状态与生命周期测试。 */
#include "ui/widgets/SerialFileTransferDialog.h"
#include "session/transfer/SerialFileTransferController.h"
#include "ElaApplication.h"
#include "ElaComboBox.h"
#include "ElaLineEdit.h"
#include "ElaProgressBar.h"
#include "ElaPushButton.h"
#include "ElaText.h"
#include <QApplication>
#include <QPointer>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

namespace {

/** @brief 通过测试角色定位控件，不改变 Ela 样式使用的对象名称。 */
template<class Widget>
Widget* transferControl(QWidget& dialog, const char* role)
{
    for (auto* widget : dialog.findChildren<Widget*>()) {
        if (widget->property("serialTransferRole").toString() == QLatin1String(role))
            return widget;
    }
    return nullptr;
}

} // namespace

class SerialFileTransferDialogTests final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { eApp->init(); }

    void protocolControlsFollowReceiveMode()
    {
        SerialFileTransferController controller({});
        SerialFileTransferDialog dialog(&controller,
            NovaTerm::FileTransfer::Direction::Receive);
        auto* protocol = transferControl<ElaComboBox>(dialog, "transferProtocol");
        auto* size = transferControl<ElaLineEdit>(dialog, "transferExpectedSize");
        QVERIFY(protocol);
        QVERIFY(size);
        QCOMPARE(protocol->count(), 5);
        QCOMPARE(protocol->objectName(), QStringLiteral("ElaComboBox"));
        QCOMPARE(size->objectName(), QStringLiteral("ElaLineEdit"));
        QCOMPARE(transferControl<ElaPushButton>(dialog, "transferStart")->objectName(),
                 QStringLiteral("ElaPushButton"));
        QCOMPARE(transferControl<ElaProgressBar>(dialog, "transferProgress")->objectName(),
                 QStringLiteral("ElaProgressBar"));
        protocol->setCurrentIndex(0);
        QVERIFY(!size->isHidden());
        protocol->setCurrentIndex(4);
        QVERIFY(size->isHidden());
        dialog.setDirection(NovaTerm::FileTransfer::Direction::Send);
        protocol->setCurrentIndex(0);
        QVERIFY(size->isHidden());
        QVERIFY(!dialog.isModal());
    }

    void preparingLocksSetupAndUnknownLengthIsBusy()
    {
        SerialFileTransferController controller({});
        SerialFileTransferDialog dialog(&controller,
            NovaTerm::FileTransfer::Direction::Send);
        SerialTransferProgress progress;
        progress.active = true;
        progress.preparing = true;
        progress.transferredBytes = 17;
        controller.activeChanged(true);
        controller.progressChanged(progress);
        QVERIFY(!transferControl<ElaComboBox>(dialog, "transferProtocol")->isEnabled());
        QVERIFY(!transferControl<ElaComboBox>(dialog, "transferDirection")->isEnabled());
        QVERIFY(!transferControl<ElaPushButton>(dialog, "transferStart")->isEnabled());
        QVERIFY(transferControl<ElaPushButton>(dialog, "transferCancel")->isEnabled());
        auto* bar = transferControl<ElaProgressBar>(dialog, "transferProgress");
        QCOMPARE(bar->minimum(), 0);
        QCOMPARE(bar->maximum(), 0);
        progress.preparing = false;
        progress.totalBytes = 0xffffffffULL;
        progress.transferredBytes = 0x80000000ULL;
        controller.progressChanged(progress);
        QCOMPARE(bar->maximum(), 1000);
        QVERIFY(bar->value() >= 499 && bar->value() <= 501);
        controller.activeChanged(false);
        QVERIFY(transferControl<ElaComboBox>(dialog, "transferProtocol")->isEnabled());
        QVERIFY(!transferControl<ElaPushButton>(dialog, "transferCancel")->isEnabled());
    }

    void controllerDestructionClosesWindowSafely()
    {
        auto controller = std::make_unique<SerialFileTransferController>(
            SerialFileTransferController::Channel{});
        SerialFileTransferDialog dialog(controller.get(),
            NovaTerm::FileTransfer::Direction::Receive);
        dialog.show();
        QVERIFY(dialog.isVisible());
        controller.reset();
        QVERIFY(!dialog.isVisible());
        QVERIFY(!transferControl<ElaPushButton>(dialog, "transferStart")->isEnabled());
        dialog.reject();
    }

    void existingXmodemTargetDoesNotReserveChannel()
    {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const auto path = folder.filePath("existing.bin");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("preserve"), 8);
        file.close();
        int reservations = 0;
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; };
        channel.reserve = [&reservations](quint64) { ++reservations; return true; };
        SerialFileTransferController controller(std::move(channel));
        SerialFileTransferDialog dialog(&controller,
            NovaTerm::FileTransfer::Direction::Receive);
        transferControl<ElaComboBox>(dialog, "transferProtocol")->setCurrentIndex(0);
        transferControl<ElaLineEdit>(dialog, "transferPath")->setText(path);
        transferControl<ElaPushButton>(dialog, "transferStart")->click();
        QCOMPARE(reservations, 0);
        QVERIFY(!controller.isActive());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("preserve"));
    }

    void closingDuringPreparationCancels()
    {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; };
        channel.reserve = [](quint64) { return true; };
        channel.coreIdle = [] { return false; };
        channel.pendingWriteBytes = [] { return qint64(0); };
        channel.write = [](QByteArrayView bytes) { return bytes.size(); };
        channel.release = [] {};
        SerialFileTransferController controller(std::move(channel));
        SerialFileTransferDialog dialog(&controller,
            NovaTerm::FileTransfer::Direction::Receive);
        transferControl<ElaLineEdit>(dialog, "transferPath")->setText(folder.path());
        dialog.show();
        transferControl<ElaPushButton>(dialog, "transferStart")->click();
        QVERIFY(controller.isActive());
        QVERIFY(controller.progress().preparing);
        QVERIFY(!transferControl<ElaPushButton>(dialog, "transferBrowse")->isEnabled());
        dialog.reject();
        QVERIFY(!dialog.isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.isActive(), 5000);
    }
};

QTEST_MAIN(SerialFileTransferDialogTests)
#include "SerialFileTransferDialogTests.moc"
