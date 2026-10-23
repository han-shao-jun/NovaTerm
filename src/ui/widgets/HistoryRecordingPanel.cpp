#include "HistoryRecordingPanel.h"
#include "ui/models/HistoryCaptureModel.h"
#include <ElaPushButton.h>
#include <ElaSpinBox.h>
#include <ElaTableView.h>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QItemSelectionModel>
#include <QSignalBlocker>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QRegularExpressionValidator>
#include <QDialog>

using namespace NovaTerm::Recording;
HistoryRecordingPanel::HistoryRecordingPanel(HistoryService* service, QWidget* parent)
    : ElaDialog(parent), _model(new HistoryCaptureModel(service, this)) {
    setWindowTitle(tr("History recording")); setModal(false); setIsStayTop(false); setIsDefaultClosed(false);
    setWindowButtonFlags(ElaAppBarType::CloseButtonHint); resize(1000, 640); setMinimumSize(760, 500);
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
    _share = new ElaPushButton(tr("Share history…"), this); _share->setProperty("historyCaptureRole", "historyShare");
    connect(_share,&ElaPushButton::clicked,this,&HistoryRecordingPanel::manageSharing);
    for (auto* button : {_start, _stop, _keep, _remove, _share}) actions->addWidget(button);
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
    _share->setEnabled(_sharing && bool(selected) && !_model->busy());
}
void HistoryRecordingPanel::setSharingPort(HistorySharingPort* port){_sharing=port;updateActions();}
void HistoryRecordingPanel::manageSharing(){
    const auto selected=_model->capture(selectedRow());if(!_sharing||!selected)return;
    const auto clients=_sharing->clients();if(clients.empty()){_status->setText(tr("Add an MCP client in MCP settings first."));return;}
    const auto capture=selected->id;
    QDialog dialog(this);dialog.setWindowTitle(tr("Share recorded history"));auto* layout=new QVBoxLayout(&dialog);
    auto* note=new QLabel(tr("This permission allows the selected client to read and search the chosen recording even after its terminal is closed. Terminal permissions are separate. Token rotation requires sharing again."),&dialog);note->setWordWrap(true);layout->addWidget(note);
    auto* form=new QFormLayout;auto* client=new QComboBox(&dialog);for(const auto& c:clients)client->addItem(c.label,c.id);form->addRow(tr("Client"),client);
    const auto edit=[&]{auto* e=new QLineEdit(&dialog);e->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9]{1,20}"),e));return e;};
    auto* first=edit();auto* last=edit();auto* from=edit();auto* to=edit();
    form->addRow(tr("First record"),first);form->addRow(tr("Last record (0 = future committed records)"),last);
    form->addRow(tr("From timestamp (Unix ms, 0 = any)"),from);form->addRow(tr("To timestamp (Unix ms, 0 = any)"),to);
    auto* persistent=new QCheckBox(tr("Keep this sharing permission after restart"),&dialog);form->addRow(persistent);layout->addLayout(form);
    const auto refresh=[&]{const auto g=_sharing->grant(client->currentData().toString(),capture);
        first->setText(QString::number(g?g->first:1));last->setText(QString::number(g?g->last:selected->highWaterRecordId));
        from->setText(QString::number(g?g->fromMs:0));to->setText(QString::number(g?g->toMs:0));persistent->setChecked(g&&g->persistent);};
    connect(client,&QComboBox::currentIndexChanged,&dialog,refresh);refresh();
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,&dialog);auto* revoke=buttons->addButton(tr("Revoke"),QDialogButtonBox::DestructiveRole);layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(revoke,&QPushButton::clicked,&dialog,[&]{if(_sharing->revoke(client->currentData().toString(),capture)){_status->setText(tr("History sharing revoked."));dialog.accept();}});
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{bool a,b,c,d;ResourceGrant g;g.principal=client->currentData().toString();g.capture=capture;
        g.first=first->text().toULongLong(&a);g.last=last->text().toULongLong(&b);g.fromMs=from->text().toLongLong(&c);g.toMs=to->text().toLongLong(&d);g.persistent=persistent->isChecked();
        if(a&&b&&c&&d&&_sharing->share(g)){_status->setText(tr("History sharing saved."));dialog.accept();}
        else QMessageBox::warning(&dialog,tr("History sharing"),tr("Cannot save sharing. Check the record/time range and local storage."));});
    dialog.resize(640,360);dialog.exec();
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
