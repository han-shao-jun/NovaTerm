#pragma once
#include "recording/HistoryService.h"
#include <QAbstractTableModel>
#include <QPointer>

class HistoryCaptureModel final : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit HistoryCaptureModel(NovaTerm::Recording::HistoryService*, QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex&, int role = Qt::DisplayRole) const override;
    QVariant headerData(int, Qt::Orientation, int role = Qt::DisplayRole) const override;
    std::optional<NovaTerm::Recording::CaptureDescriptor> capture(int row) const;
    void start(NovaTerm::Recording::RecordingIntent);
    void stop(int row);
    void remove(int row);
    void retain(int row, bool);
    void configure(NovaTerm::Recording::RetentionSettings);
    NovaTerm::Recording::RetentionSettings settings() const;
    bool busy() const { return _busy; }
    bool settingsReady() const { return _service && _service->ready(); }
signals:
    void statusChanged(const QString& message);
    void settingsChanged();
    void settingsApplied(bool success);
private:
    QPointer<NovaTerm::Recording::HistoryService> _service;
    std::vector<NovaTerm::Recording::CaptureDescriptor> _rows;
    bool _busy{false};
    void refresh();
    NovaTerm::Recording::HistoryService::Done completion();
};
