/**
 * @file SerialFileTransferDialog.h
 * @brief 串口手动文件传输窗口：设置、进度与取消。
 */
#pragma once

#include "session/transfer/SerialTransferTypes.h"
#include <ElaDialog.h>
#include <QPointer>

class ElaComboBox;
class ElaLineEdit;
class ElaProgressBar;
class ElaPushButton;
class ElaText;
class QCloseEvent;
class SerialFileTransferController;

/** @brief 非模态传输窗口，控制器由 Session 持有，本窗口只保存观察指针。 */
class SerialFileTransferDialog final : public ElaDialog
{
    Q_OBJECT
public:
    explicit SerialFileTransferDialog(SerialFileTransferController* controller,
        NovaTerm::FileTransfer::Direction direction, QWidget* parent = nullptr);
    /** @brief 空闲时切换方向；活动传输的设置保持锁定。 */
    void setDirection(NovaTerm::FileTransfer::Direction direction);
    /** @brief 关闭或按 Esc 时取消活动传输，然后隐藏窗口。 */
    void reject() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    [[nodiscard]] bool isXmodem() const;
    [[nodiscard]] NovaTerm::FileTransfer::Direction direction() const;
    void updateSetup();
    void setActive(bool active);
    void choosePath();
    void startTransfer();
    void updateProgress(const SerialTransferProgress& progress);
    void retranslateUi();
    void showError(const QString& message);

    QPointer<SerialFileTransferController> _controller;
    QStringList _files;
    SerialTransferProgress _progress;
    bool _active{false};
    ElaComboBox* _protocol{nullptr};
    ElaComboBox* _direction{nullptr};
    ElaLineEdit* _path{nullptr};
    ElaLineEdit* _expectedSize{nullptr};
    ElaPushButton* _browse{nullptr};
    ElaPushButton* _start{nullptr};
    ElaPushButton* _cancel{nullptr};
    ElaPushButton* _close{nullptr};
    ElaProgressBar* _progressBar{nullptr};
    ElaText* _protocolLabel{nullptr};
    ElaText* _directionLabel{nullptr};
    ElaText* _pathLabel{nullptr};
    ElaText* _sizeLabel{nullptr};
    ElaText* _instructions{nullptr};
    ElaText* _note{nullptr};
    ElaText* _status{nullptr};
    ElaText* _currentFile{nullptr};
    ElaText* _details{nullptr};
};
