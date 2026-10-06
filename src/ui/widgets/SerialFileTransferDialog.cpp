/** @file SerialFileTransferDialog.cpp @brief 串口手动文件传输窗口实现。 */
#include "SerialFileTransferDialog.h"
#include "MessagePrompts.h"
#include "session/transfer/SerialFileTransferController.h"
#include "service/LanguageManager.h"
#include "ElaComboBox.h"
#include "ElaLineEdit.h"
#include "ElaProgressBar.h"
#include "ElaPushButton.h"
#include "ElaText.h"
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLocale>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>
#include <algorithm>

SerialFileTransferDialog::SerialFileTransferDialog(
    SerialFileTransferController* controller,
    NovaTerm::FileTransfer::Direction initialDirection, QWidget* parent)
    : ElaDialog(parent), _controller(controller)
{
    setModal(false);
    setIsStayTop(false);
    setIsDefaultClosed(false);
    setWindowButtonFlags(ElaAppBarType::CloseButtonHint);
    setMinimumWidth(460);
    resize(580, 540);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 16, 24, 16);
    layout->setSpacing(12);
    auto* form = new QGridLayout;
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(10);
    form->setColumnStretch(1, 1);
    _protocolLabel = new ElaText(this);
    _protocol = new ElaComboBox(this);
    _protocol->setProperty("serialTransferRole", QStringLiteral("transferProtocol"));
    _protocol->addItem(QString{}, static_cast<int>(SerialTransferProtocol::XmodemChecksum));
    _protocol->addItem(QString{}, static_cast<int>(SerialTransferProtocol::XmodemCrc));
    _protocol->addItem(QString{}, static_cast<int>(SerialTransferProtocol::Xmodem1K));
    _protocol->addItem(QString{}, static_cast<int>(SerialTransferProtocol::Ymodem));
    _protocol->addItem(QString{}, static_cast<int>(SerialTransferProtocol::Zmodem));
    _protocol->setCurrentIndex(4);
    _protocolLabel->setBuddy(_protocol);
    form->addWidget(_protocolLabel, 0, 0);
    form->addWidget(_protocol, 0, 1, 1, 2);
    _directionLabel = new ElaText(this);
    _direction = new ElaComboBox(this);
    _direction->setProperty("serialTransferRole", QStringLiteral("transferDirection"));
    _direction->addItem(QString{}, static_cast<int>(NovaTerm::FileTransfer::Direction::Send));
    _direction->addItem(QString{}, static_cast<int>(NovaTerm::FileTransfer::Direction::Receive));
    _direction->setCurrentIndex(initialDirection == NovaTerm::FileTransfer::Direction::Send ? 0 : 1);
    _directionLabel->setBuddy(_direction);
    form->addWidget(_directionLabel, 1, 0);
    form->addWidget(_direction, 1, 1, 1, 2);
    _pathLabel = new ElaText(this);
    _path = new ElaLineEdit(this);
    _path->setProperty("serialTransferRole", QStringLiteral("transferPath"));
    _path->setMinimumWidth(180);
    _pathLabel->setBuddy(_path);
    _browse = new ElaPushButton(this);
    _browse->setProperty("serialTransferRole", QStringLiteral("transferBrowse"));
    _browse->setMinimumHeight(36);
    form->addWidget(_pathLabel, 2, 0);
    form->addWidget(_path, 2, 1);
    form->addWidget(_browse, 2, 2);
    _sizeLabel = new ElaText(this);
    _expectedSize = new ElaLineEdit(this);
    _expectedSize->setProperty("serialTransferRole", QStringLiteral("transferExpectedSize"));
    _expectedSize->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9]{0,10}")), _expectedSize));
    _sizeLabel->setBuddy(_expectedSize);
    form->addWidget(_sizeLabel, 3, 0);
    form->addWidget(_expectedSize, 3, 1, 1, 2);
    layout->addLayout(form);

    _instructions = new ElaText(this);
    _instructions->setWordWrap(true);
    _instructions->setTextStyle(ElaTextType::BodyStrong);
    layout->addWidget(_instructions);
    _note = new ElaText(this);
    _note->setWordWrap(true);
    _note->setTextStyle(ElaTextType::Caption);
    layout->addWidget(_note);
    _status = new ElaText(this);
    _status->setProperty("serialTransferRole", QStringLiteral("transferStatus"));
    _status->setWordWrap(true);
    layout->addWidget(_status);
    _progressBar = new ElaProgressBar(this);
    _progressBar->setProperty("serialTransferRole", QStringLiteral("transferProgress"));
    _progressBar->setRange(0, 1000);
    _progressBar->setValue(0);
    _progressBar->setTextVisible(false);
    layout->addWidget(_progressBar);
    _currentFile = new ElaText(this);
    _currentFile->setProperty("serialTransferRole", QStringLiteral("transferCurrentFile"));
    _currentFile->setWordWrap(true);
    _currentFile->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(_currentFile);
    _details = new ElaText(this);
    _details->setProperty("serialTransferRole", QStringLiteral("transferDetails"));
    _details->setWordWrap(true);
    layout->addWidget(_details);
    layout->addStretch();

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    _start = new ElaPushButton(this);
    _start->setProperty("serialTransferRole", QStringLiteral("transferStart"));
    _start->setDefault(true);
    _cancel = new ElaPushButton(this);
    _cancel->setProperty("serialTransferRole", QStringLiteral("transferCancel"));
    _close = new ElaPushButton(this);
    for (auto* button : {_start, _cancel, _close}) {
        button->setMinimumSize(88, 36);
        buttons->addWidget(button);
    }
    layout->addLayout(buttons);
    connect(_browse, &QPushButton::clicked, this, &SerialFileTransferDialog::choosePath);
    connect(_start, &QPushButton::clicked, this, &SerialFileTransferDialog::startTransfer);
    connect(_cancel, &QPushButton::clicked, this, [this] {
        if (_controller)
            _controller->cancel();
    });
    connect(_close, &QPushButton::clicked, this, &SerialFileTransferDialog::reject);
    connect(this, &ElaDialog::closeButtonClicked, this, &SerialFileTransferDialog::reject);
    connect(_protocol, &QComboBox::currentIndexChanged,
            this, &SerialFileTransferDialog::updateSetup);
    connect(_direction, &QComboBox::currentIndexChanged, this, [this] {
        _files.clear();
        _path->clear();
        _path->setToolTip(QString{});
        _expectedSize->clear();
        updateSetup();
    });
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this] { retranslateUi(); });
    if (_controller) {
        connect(_controller, &SerialFileTransferController::activeChanged,
                this, &SerialFileTransferDialog::setActive);
        connect(_controller, &SerialFileTransferController::progressChanged,
                this, &SerialFileTransferDialog::updateProgress);
        connect(_controller, &SerialFileTransferController::finished,
                this, [this](bool success, const QString& message) {
            _status->setText(success ? tr("Transfer completed.")
                                    : tr("Transfer stopped: %1").arg(message));
            if (success) {
                _progressBar->setMinimum(0);
                _progressBar->setMaximum(1000);
                _progressBar->setValue(1000);
            }
        });
        connect(_controller, &QObject::destroyed, this, [this] {
            // destroyed 发出时控制器的实现已经析构，不再向它调用 cancel。
            _controller.clear();
            setActive(false);
            ElaDialog::reject();
        });
        _progress = _controller->progress();
        if (_controller->isActive()) {
            _direction->setCurrentIndex(
                _progress.direction == NovaTerm::FileTransfer::Direction::Send ? 0 : 1);
        }
    }
    retranslateUi();
    setActive(_controller && _controller->isActive());
}

bool SerialFileTransferDialog::isXmodem() const
{
    const auto protocol = static_cast<SerialTransferProtocol>(_protocol->currentData().toInt());
    return protocol == SerialTransferProtocol::XmodemChecksum
        || protocol == SerialTransferProtocol::XmodemCrc
        || protocol == SerialTransferProtocol::Xmodem1K;
}

NovaTerm::FileTransfer::Direction SerialFileTransferDialog::direction() const
{
    return static_cast<NovaTerm::FileTransfer::Direction>(_direction->currentData().toInt());
}

void SerialFileTransferDialog::setDirection(NovaTerm::FileTransfer::Direction value)
{
    if (!_active && (!_controller || !_controller->isActive()))
        _direction->setCurrentIndex(value == NovaTerm::FileTransfer::Direction::Send ? 0 : 1);
}

void SerialFileTransferDialog::updateSetup()
{
    const bool sending = direction() == NovaTerm::FileTransfer::Direction::Send;
    const bool receiveX = !sending && isXmodem();
    _path->setReadOnly(sending);
    _pathLabel->setText(sending ? tr("Files:")
                               : receiveX ? tr("Save as:") : tr("Folder:"));
    _path->setAccessibleName(_pathLabel->text());
    _path->setPlaceholderText(sending ? tr("Choose files to send")
        : receiveX ? tr("Full path of the new file") : tr("Choose a receiving folder"));
    _sizeLabel->setVisible(receiveX);
    _expectedSize->setVisible(receiveX);
    _instructions->setText(sending
        ? tr("On the device, start its receiving program with the same protocol, then click Start.")
        : tr("On the device, start its sending program with the same protocol, then click Start."));
    QString note = tr("Existing files will not be overwritten. Cancelling or closing an active transfer disconnects the serial port.");
    if (receiveX)
        note += QLatin1Char('\n') + tr("For XMODEM, enter the exact size to remove padding. Leave it blank to keep padding bytes.");
    _note->setText(note);
}

void SerialFileTransferDialog::setActive(bool active)
{
    _active = active;
    for (auto* control : {static_cast<QWidget*>(_protocol), static_cast<QWidget*>(_direction),
            static_cast<QWidget*>(_path), static_cast<QWidget*>(_expectedSize),
            static_cast<QWidget*>(_browse)})
        control->setEnabled(!active && _controller);
    _start->setEnabled(!active && _controller);
    _cancel->setEnabled(active && _controller);
    if (!active && _progressBar->maximum() == 0) {
        _progressBar->setMinimum(0);
        _progressBar->setMaximum(1000);
        _progressBar->setValue(0);
    }
}

void SerialFileTransferDialog::choosePath()
{
    if (!_controller || _active)
        return;
    // 原生文件选择器是 Ela 没有对应控件的例外；嵌套事件循环后必须重新判活。
    const QPointer<SerialFileTransferDialog> guard(this);
    const bool sending = direction() == NovaTerm::FileTransfer::Direction::Send;
    const bool single = isXmodem();
    QStringList files;
    QString path;
    if (sending) {
        if (single) {
            path = QFileDialog::getOpenFileName(this, tr("Choose a file to send"));
            if (!path.isEmpty())
                files.append(path);
        } else {
            files = QFileDialog::getOpenFileNames(this, tr("Choose files to send"));
        }
    } else if (single) {
        path = QFileDialog::getSaveFileName(this, tr("Choose a new receiving file"),
            _path->text(), QString{}, nullptr, QFileDialog::DontConfirmOverwrite);
    } else {
        path = QFileDialog::getExistingDirectory(this, tr("Choose a receiving folder"),
                                                _path->text());
    }
    if (!guard || !_controller || _controller->isActive())
        return;
    if (sending && !files.isEmpty()) {
        _files = files;
        QStringList names;
        for (const auto& file : files)
            names.append(QFileInfo(file).fileName());
        _path->setText(names.join(QStringLiteral("; ")));
        _path->setToolTip(files.join(QLatin1Char('\n')));
    } else if (!sending && !path.isEmpty()) {
        _path->setText(QDir::toNativeSeparators(path));
        _path->setToolTip(_path->text());
    }
}

void SerialFileTransferDialog::showError(const QString& message)
{
    _status->setText(message);
    NovaTerm::Ui::warn(this, tr("Cannot start transfer"), message);
}

void SerialFileTransferDialog::startTransfer()
{
    if (!_controller || _controller->isActive())
        return;
    SerialTransferRequest request;
    request.protocol = static_cast<SerialTransferProtocol>(_protocol->currentData().toInt());
    request.direction = direction();
    if (request.direction == NovaTerm::FileTransfer::Direction::Send) {
        if (_files.isEmpty()) {
            showError(tr("Choose at least one file to send."));
            return;
        }
        if (isXmodem() && _files.size() != 1) {
            showError(tr("XMODEM sends one file at a time. Choose a single file."));
            return;
        }
        if (_files.size() > static_cast<qsizetype>(NovaTerm::FileTransfer::MaxFiles)) {
            showError(tr("Choose no more than %1 files.").arg(NovaTerm::FileTransfer::MaxFiles));
            return;
        }
        request.files = _files;
    } else {
        request.destination = _path->text();
        if (request.destination.isEmpty()) {
            showError(tr("Choose where to save the received files."));
            return;
        }
        const QFileInfo target(request.destination);
        if (isXmodem()) {
            if (target.exists() || target.isSymLink()) {
                showError(tr("The receiving file already exists. Choose a new file name."));
                return;
            }
            const QString size = _expectedSize->text().trimmed();
            if (!size.isEmpty()) {
                bool valid = false;
                const quint64 bytes = size.toULongLong(&valid);
                if (!valid || bytes > NovaTerm::FileTransfer::MaxFileSize) {
                    showError(tr("Enter a file size from 0 to 4294967295 bytes, or leave it blank."));
                    return;
                }
                request.expectedSize = bytes;
            }
        } else if (!target.isDir()) {
            showError(tr("Choose an existing receiving folder."));
            return;
        }
    }
    const QPointer<SerialFileTransferDialog> guard(this);
    if (!_controller->start(request) && guard)
        showError(_controller ? _controller->errorString() : tr("The session is no longer available."));
}

void SerialFileTransferDialog::updateProgress(const SerialTransferProgress& progress)
{
    _progress = progress;
    _status->setText(progress.preparing ? tr("Preparing transfer…")
        : progress.status.isEmpty() ? (progress.active ? tr("Transferring…") : tr("Ready"))
                                    : progress.status);
    if (progress.active && !progress.totalBytes) {
        _progressBar->setMinimum(0);
        _progressBar->setMaximum(0);
    } else {
        // 避免 32 位进度控件截断大文件；先用浮点比值归一化到千分比。
        const int value = progress.totalBytes && *progress.totalBytes != 0
            ? static_cast<int>(1000.0L * std::min(1.0L,
                static_cast<long double>(progress.transferredBytes) / *progress.totalBytes))
            : 0;
        _progressBar->setMinimum(0);
        _progressBar->setMaximum(1000);
        _progressBar->setValue(value);
    }
    _currentFile->setText(progress.currentFile.isEmpty() ? QString{}
        : tr("File: %1").arg(progress.currentFile));
    const QLocale locale;
    const QString bytes = progress.totalBytes
        ? tr("%1 / %2 bytes").arg(locale.toString(progress.transferredBytes),
                                  locale.toString(*progress.totalBytes))
        : tr("%1 bytes (total unknown)").arg(locale.toString(progress.transferredBytes));
    _details->setText(tr("%1 · %2 files completed · %3 s elapsed")
        .arg(bytes, locale.toString(progress.completedFiles),
             locale.toString(static_cast<double>(progress.elapsedMs) / 1000.0, 'f', 1)));
}

void SerialFileTransferDialog::retranslateUi()
{
    setWindowTitle(tr("Serial File Transfer"));
    _protocolLabel->setText(tr("Protocol:"));
    _protocol->setAccessibleName(tr("Protocol:"));
    _protocol->setItemText(0, tr("XMODEM (Checksum)"));
    _protocol->setItemText(1, tr("XMODEM (CRC)"));
    _protocol->setItemText(2, tr("XMODEM-1K"));
    _protocol->setItemText(3, tr("YMODEM"));
    _protocol->setItemText(4, tr("ZMODEM"));
    _directionLabel->setText(tr("Direction:"));
    _direction->setAccessibleName(tr("Direction:"));
    _direction->setItemText(0, tr("Send"));
    _direction->setItemText(1, tr("Receive"));
    _sizeLabel->setText(tr("Exact size (bytes):"));
    _expectedSize->setAccessibleName(tr("Exact size (bytes):"));
    _expectedSize->setPlaceholderText(tr("Optional, 0–4294967295"));
    _browse->setText(tr("Browse…"));
    _start->setText(tr("Start"));
    _cancel->setText(tr("Cancel Transfer"));
    _close->setText(tr("Close"));
    updateSetup();
    updateProgress(_progress);
}

void SerialFileTransferDialog::reject()
{
    const QPointer<SerialFileTransferDialog> guard(this);
    if (_controller && _controller->isActive())
        _controller->cancel();
    if (guard)
        ElaDialog::reject();
}

void SerialFileTransferDialog::closeEvent(QCloseEvent* event)
{
    const QPointer<SerialFileTransferDialog> guard(this);
    if (_controller && _controller->isActive())
        _controller->cancel();
    if (guard)
        ElaDialog::closeEvent(event);
}
