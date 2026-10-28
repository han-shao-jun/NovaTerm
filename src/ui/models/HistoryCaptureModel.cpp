#include "HistoryCaptureModel.h"
#include <QDateTime>
#include <QLocale>

using namespace NovaTerm::Recording;
HistoryCaptureModel::HistoryCaptureModel(HistoryService* service, QObject* parent)
    : QAbstractTableModel(parent), _service(service) {
    if (service) connect(service, &HistoryService::changed, this, &HistoryCaptureModel::refresh);
    refresh();
}
void HistoryCaptureModel::refresh() {
    auto next = _service ? _service->captures() : std::vector<CaptureDescriptor>{};
    bool sameIds = next.size() == _rows.size();
    if (sameIds) for (size_t i = 0; i < next.size(); ++i) if (next[i].id != _rows[i].id) sameIds = false;
    if (sameIds) {
        _rows = std::move(next);
        if (!_rows.empty()) emit dataChanged(index(0, 0), index(int(_rows.size()) - 1, columnCount() - 1), {Qt::DisplayRole});
    } else { beginResetModel(); _rows = std::move(next); endResetModel(); }
    if (_service && !_service->error().isEmpty()) emit statusChanged(_service->error());
    emit settingsChanged();
}
int HistoryCaptureModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : int(_rows.size()); }
int HistoryCaptureModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : 6; }
QVariant HistoryCaptureModel::headerData(int column, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    const QStringList names{tr("Capture / started"), tr("Connection"), tr("State / integrity"), tr("Stored"), tr("Known missing"), tr("Keep")};
    return column >= 0 && column < names.size() ? names[column] : QVariant{};
}
QVariant HistoryCaptureModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= int(_rows.size())) return {};
    const auto& d = _rows[size_t(index.row())];
    if (role == Qt::ToolTipRole) return d.id.text() + "\n" + d.sessionId.toString(QUuid::WithoutBraces);
    if (role != Qt::DisplayRole) return {};
    switch (index.column()) {
    case 0: return QDateTime::fromMSecsSinceEpoch(d.createdMs).toString("yyyy-MM-dd HH:mm:ss") + "\n" + d.id.text().left(12);
    case 1: return d.sessionId.toString(QUuid::WithoutBraces).left(8) + " / " + QString::number(d.connectionGeneration);
    case 2: {
        QString state;
        if (d.active) state = tr("Recording — unsealed");
        else if (d.integrity == Integrity::Lossless) state = tr("Stopped — complete");
        else if (d.integrity == Integrity::KnownGap) state = tr("Stopped — known gaps");
        else if (d.integrity == Integrity::RecoveredUnsealed) state = tr("Recovered — unknown tail");
        else state = tr("Stopped — unknown tail");
        return d.stopReason.isEmpty() ? state : state + "\n" + d.stopReason;
    }
    case 3: return QLocale().formattedDataSize(qint64(d.bytes));
    case 4: return QLocale().formattedDataSize(qint64(d.gapBytes));
    case 5: return d.retained ? tr("Kept") : tr("Automatic retention");
    default: return {};
    }
}
std::optional<CaptureDescriptor> HistoryCaptureModel::capture(int row) const {
    if (row < 0 || row >= int(_rows.size())) return {};
    return _rows[size_t(row)];
}
HistoryService::Done HistoryCaptureModel::completion() {
    _busy = true; emit statusChanged(tr("Working…")); QPointer<HistoryCaptureModel> guard(this);
    return [guard](RecordingResult result) {
        if (!guard) return;
        guard->_busy = false;
        guard->refresh(); if (guard) emit guard->statusChanged(result.success ? tr("Saved") : result.error);
    };
}
void HistoryCaptureModel::start(RecordingIntent intent) {
    if (!_service || _busy) return;
    if (!_service->start(intent, completion())) { _busy = false; emit statusChanged(tr("Recording request queue is full")); }
}
void HistoryCaptureModel::stop(int row) {
    const auto d = capture(row); if (_service && d && d->active) _service->stop({d->sessionId, d->connectionGeneration});
}
void HistoryCaptureModel::remove(int row) {
    const auto d = capture(row); if (!_service || !d || d->active || _busy) return;
    if (!_service->remove(d->id, completion())) { _busy = false; emit statusChanged(tr("Recording request queue is full")); }
}
void HistoryCaptureModel::retain(int row, bool keep) {
    const auto d = capture(row); if (!_service || !d || _busy) return;
    if (!_service->retain(d->id, keep, completion())) { _busy = false; emit statusChanged(tr("Recording request queue is full")); }
}
void HistoryCaptureModel::configure(RetentionSettings settings) {
    if (!_service || _busy) return;
    QPointer<HistoryCaptureModel> guard(this); auto complete = completion();
    if (!_service->configure(settings, [guard, complete = std::move(complete)](RecordingResult result) {
        const bool success = result.success; complete(std::move(result)); if (guard) emit guard->settingsApplied(success);
    })) { _busy = false; emit statusChanged(tr("Invalid limits or request queue is full")); }
}
RetentionSettings HistoryCaptureModel::settings() const { return _service ? _service->settings() : RetentionSettings{}; }
