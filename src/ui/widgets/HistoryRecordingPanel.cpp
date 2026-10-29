#include "HistoryRecordingPanel.h"
#include "ui/models/HistoryCaptureModel.h"
#include <ElaPushButton.h>
#include <ElaSpinBox.h>
#include <ElaTableView.h>
#include <ElaCheckBox.h>
#include <ElaLineEdit.h>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QItemSelectionModel>
#include <QSignalBlocker>
#include <QLineEdit>
#include <QRegularExpressionValidator>
#include <QFileDialog>
#include <QPointer>

using namespace NovaTerm::Recording;
HistoryRecordingPanel::HistoryRecordingPanel(HistoryService* service, QWidget* parent)
    : ElaDialog(parent), _model(new HistoryCaptureModel(service, this)) {
    setWindowTitle(tr("History recording")); setModal(false); setIsStayTop(false); setIsDefaultClosed(false);
    setWindowButtonFlags(ElaAppBarType::CloseButtonHint); resize(1000, 640); setMinimumSize(760, 500);
    // 面板由终端页复用；关闭只隐藏窗口，不停止录制或销毁界面状态。
    connect(this, &ElaDialog::closeButtonClicked, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(24, 16, 24, 16);
    auto* note = new QLabel(tr("Recording starts only when you choose Start. Sharing history requires separate permission. "
        "Remote output may contain passwords or other secrets; automatic masking is unavailable. File transfers are omitted and marked as gaps."), this);
    note->setWordWrap(true); layout->addWidget(note);
    _targetLabel = new QLabel(this); layout->addWidget(_targetLabel);
    _table = new ElaTableView(this); _table->setProperty("historyCaptureRole", "historyCaptures"); _table->setModel(_model);
    _table->setSelectionBehavior(QAbstractItemView::SelectRows); _table->setSelectionMode(QAbstractItemView::SingleSelection);
    _table->setEditTriggers(QAbstractItemView::NoEditTriggers); _table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    _table->horizontalHeader()->setStretchLastSection(true); _table->verticalHeader()->setDefaultSectionSize(54); layout->addWidget(_table, 1);
    auto* actions = new QHBoxLayout;
    _start = new ElaPushButton(tr("Start for current terminal"), this); _start->setProperty("historyCaptureRole", "historyStart");
    _stop = new ElaPushButton(tr("Stop selected"), this); _stop->setProperty("historyCaptureRole", "historyStop");
    _keep = new ElaPushButton(tr("Keep / release selected"), this);
    _remove = new ElaPushButton(tr("Delete selected"), this); _remove->setProperty("historyCaptureRole", "historyDelete");
    for (auto* button : {_start, _stop, _keep, _remove}) actions->addWidget(button);
    layout->addLayout(actions);
    auto* limits = new QHBoxLayout;
    _global = new ElaSpinBox(this); _global->setRange(1, 65536); _global->setSuffix(tr(" MiB total"));
    _capture = new ElaSpinBox(this); _capture->setRange(1, 16384); _capture->setSuffix(tr(" MiB / capture"));
    _days = new ElaSpinBox(this); _days->setRange(1, 365); _days->setSuffix(tr(" days"));
    _global->setFixedWidth(180); _capture->setFixedWidth(190); _days->setFixedWidth(115);
    _apply = new ElaPushButton(tr("Apply retention limits"), this);
    for (auto* spin : {_global, _capture, _days}) limits->addWidget(spin);
    limits->addWidget(_apply); layout->addLayout(limits);
    auto* retentionNote = new QLabel(tr("Kept captures are excluded from automatic cleanup and still count toward the total limit. "
        "If space cannot be reclaimed, recording stops with an unknown tail."), this);
    retentionNote->setWordWrap(true); layout->addWidget(retentionNote);
    _status = new QLabel(this); _status->setWordWrap(true); layout->addWidget(_status);
    _sharingPanel = new QWidget(this);
    auto* sharing = new QVBoxLayout(_sharingPanel); sharing->setContentsMargins(0, 8, 0, 0);
    auto* sharingNote = new QLabel(tr("History and imported files are shared with all AI connections using the common MCP token, independently of terminal permissions. Closing a terminal does not revoke this permission. An end record of 0 includes future committed records."), _sharingPanel);
    sharingNote->setWordWrap(true); sharing->addWidget(sharingNote);
    auto* shareRange = new QHBoxLayout;
    _shareFirst = new ElaLineEdit(_sharingPanel); _shareFirst->setText("1"); _shareFirst->setProperty("historyCaptureRole", "historyShareFirst");
    _shareLast = new ElaLineEdit(_sharingPanel); _shareLast->setText("0"); _shareLast->setProperty("historyCaptureRole", "historyShareLast");
    for (auto* field : {_shareFirst, _shareLast}) { field->setMaximumWidth(120); field->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9]{1,20}"), field)); }
    shareRange->addWidget(new QLabel(tr("From record"), _sharingPanel)); shareRange->addWidget(_shareFirst);
    shareRange->addWidget(new QLabel(tr("Through record (0: high water)"), _sharingPanel)); shareRange->addWidget(_shareLast); sharing->addLayout(shareRange);
    auto* shareActions = new QHBoxLayout;
    _sharePersistent = new ElaCheckBox(tr("Keep permission after restart"), _sharingPanel); _sharePersistent->setProperty("historyCaptureRole", "historySharePersistent");
    _share = new ElaPushButton(tr("Share selected"), _sharingPanel); _share->setProperty("historyCaptureRole", "historyShare");
    _unshare = new ElaPushButton(tr("Revoke history permission"), _sharingPanel); _unshare->setProperty("historyCaptureRole", "historyUnshare");
    shareActions->addWidget(_sharePersistent, 1); shareActions->addWidget(_share); shareActions->addWidget(_unshare); sharing->addLayout(shareActions);
    _shareStatus = new QLabel(_sharingPanel); _shareStatus->setWordWrap(true); sharing->addWidget(_shareStatus);
    auto* imports = new QHBoxLayout;
    _importFile = new ElaPushButton(tr("Share local log file…"), _sharingPanel);
    _importFile->setProperty("historyCaptureRole", "historyImportFile");
    _clearImports = new ElaPushButton(tr("Revoke imported files"), _sharingPanel);
    _clearImports->setProperty("historyCaptureRole", "historyClearImports");
    imports->addWidget(_importFile); imports->addWidget(_clearImports); imports->addStretch(); sharing->addLayout(imports);
    connect(_importFile, &ElaPushButton::clicked, this, [this] {
        if (!_sharing.importFile || !sharingCurrent()) return;
        const auto sharing = _sharing;
        const QPointer<HistoryRecordingPanel> self(this);
        const auto selected = QFileDialog::getOpenFileName(this, tr("Explicitly share a local log (up to 16 MiB)"));
        if (!self || selected.isEmpty()) return;
        if (!sharingCurrent() || _sharing.principalId != sharing.principalId || _sharing.identityVersion != sharing.identityVersion
            || (sharing.isCurrent && !sharing.isCurrent())) {
            updateActions(); _shareStatus->setText(tr("MCP access changed. Select the file again to share it.")); return;
        }
        sharing.importFile(sharing.principalId, selected, [self, principal = sharing.principalId, identity = sharing.identityVersion](QString message) {
            if (self && self->sharingCurrent() && self->_sharing.principalId == principal && self->_sharing.identityVersion == identity)
                self->_shareStatus->setText(std::move(message));
        });
    });
    connect(_clearImports, &ElaPushButton::clicked, this, [this] {
        if (_sharing.clearImports && sharingCurrent()) { _sharing.clearImports(_sharing.principalId); _shareStatus->setText(tr("Imported files are no longer shared with AI connections.")); }
    });
    layout->addWidget(_sharingPanel); _sharingPanel->hide();
    connect(_share, &ElaPushButton::clicked, this, [this] {
        const auto selected = _model->capture(selectedRow()); if (!selected || !_sharing.share || !sharingCurrent()) return;
        bool validFirst = false, validLast = false;
        const auto first = _shareFirst->text().toULongLong(&validFirst), last = _shareLast->text().toULongLong(&validLast);
        if (!validFirst || !validLast || first == 0 || (last && last < first)) { _shareStatus->setText(tr("Enter a valid inclusive record range (first record is at least 1).")); return; }
        const auto error = _sharing.share(_sharing.principalId, selected->id, first, last, _sharePersistent->isChecked());
        updateActions(); if (!error.isEmpty()) _shareStatus->setText(error);
    });
    connect(_unshare, &ElaPushButton::clicked, this, [this] {
        const auto selected = _model->capture(selectedRow()); if (!selected || !_sharing.revoke || !sharingCurrent()) return;
        const auto error = _sharing.revoke(_sharing.principalId, selected->id);
        updateActions(); if (!error.isEmpty()) _shareStatus->setText(error);
    });
    connect(_model, &HistoryCaptureModel::statusChanged, this, [this](const QString& message) { _status->setText(message); syncSettings(); });
    connect(_model, &QAbstractItemModel::modelReset, this, &HistoryRecordingPanel::updateActions);
    connect(_model, &QAbstractItemModel::dataChanged, this, [this] { updateActions(); });
    connect(_model, &HistoryCaptureModel::settingsChanged, this, &HistoryRecordingPanel::syncSettings);
    connect(_model, &HistoryCaptureModel::settingsApplied, this, [this](bool success) {
        if (success) { _settingsEdited = false; syncSettings(); }
    });
    for (auto* spin : {_global, _capture, _days}) {
        connect(spin, &QSpinBox::valueChanged, this, [this] { _settingsEdited = true; });
        if (auto* edit = spin->findChild<QLineEdit*>())
            connect(edit, &QLineEdit::textEdited, this, [this] { _settingsEdited = true; });
    }
    connect(_table->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] { updateActions(); });
    connect(_start, &ElaPushButton::clicked, this, [this] {
        if (_resolveTarget) {
            _target = _resolveTarget();
            if (_target.sessionId.isNull()) { _connected = false; updateActions(); return; }
        }
        _model->start(_target); updateActions();
    });
    connect(_stop, &ElaPushButton::clicked, this, [this] { _model->stop(selectedRow()); });
    connect(_keep, &ElaPushButton::clicked, this, [this] { const auto d = _model->capture(selectedRow()); if (d) _model->retain(selectedRow(), !d->retained); });
    connect(_remove, &ElaPushButton::clicked, this, [this] {
        const auto d = _model->capture(selectedRow()); if (!d || d->active) return;
        if (QMessageBox::question(this, tr("Delete recording"), tr("Delete this recording and its derived files?\n%1").arg(d->id.text())) == QMessageBox::Yes) {
            // A modal event loop may refresh/sort the catalog; resolve the immutable id again.
            for (int row = 0; row < _model->rowCount(); ++row) if (_model->capture(row)->id == d->id) { _model->remove(row); break; }
        }
    });
    connect(_apply, &ElaPushButton::clicked, this, [this] {
        _model->configure({quint64(_global->value()) * 1024 * 1024, quint64(_capture->value()) * 1024 * 1024,
            qint64(_days->value()) * 24 * 60 * 60 * 1000});
    });
    syncSettings(); updateActions();
}
int HistoryRecordingPanel::selectedRow() const { return _table->currentIndex().row(); }
void HistoryRecordingPanel::setSharingActions(HistorySharingActions actions) {
    _sharing = std::move(actions);
    _sharingPanel->setVisible(bool(_sharing.share)); updateActions();
}
bool HistoryRecordingPanel::sharingCurrent() const {
    return !_sharing.principalId.isEmpty() && _sharing.identityVersion && (!_sharing.isCurrent || _sharing.isCurrent());
}
void HistoryRecordingPanel::setRecordingTarget(RecordingIntent intent, bool connected, const QString& title) {
    _target = intent; _connected = connected; _targetLabel->setText(tr("Current terminal: %1").arg(title)); updateActions();
}
void HistoryRecordingPanel::updateActions() {
    bool active = false; for (int row = 0; row < _model->rowCount(); ++row) {
        const auto d = _model->capture(row); if (d->active && d->sessionId == _target.sessionId) active = true;
    }
    const auto selected = _model->capture(selectedRow());
    _start->setEnabled(_model->settingsReady() && _connected && _target.sessionId != QUuid{} && !active && !_model->busy());
    _stop->setEnabled(selected && selected->active); _remove->setEnabled(selected && !selected->active && !_model->busy());
    _keep->setEnabled(bool(selected) && !_model->busy());
    const bool current = sharingCurrent();
    const bool canShare = selected && current && !_model->busy();
    _share->setEnabled(canShare && bool(_sharing.share)); _unshare->setEnabled(canShare && bool(_sharing.revoke));
    _importFile->setEnabled(current && bool(_sharing.importFile));
    _clearImports->setEnabled(current && bool(_sharing.clearImports));
    if (canShare && _sharing.status) _shareStatus->setText(_sharing.status(_sharing.principalId, selected->id));
    else _shareStatus->setText(current ? tr("Select a recording to share with all AI connections.") : tr("Enable MCP access before sharing history or imported files."));
}
void HistoryRecordingPanel::syncSettings() {
    const bool ready = _model->settingsReady();
    for (auto* spin : {_global, _capture, _days}) spin->setEnabled(ready && !_model->busy());
    _apply->setEnabled(ready && !_model->busy());
    if (ready && !_settingsEdited) {
        const QSignalBlocker global(_global), capture(_capture), days(_days);
        const auto settings = _model->settings();
        _global->setValue(int(settings.globalBytes / (1024 * 1024)));
        _capture->setValue(int(settings.captureBytes / (1024 * 1024)));
        _days->setValue(int(settings.retentionMs / (24ll * 60 * 60 * 1000)));
    }
    updateActions();
}
