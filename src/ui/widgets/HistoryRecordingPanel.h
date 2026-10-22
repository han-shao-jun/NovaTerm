#pragma once
#include "history/HistoryTypes.h"
#include <ElaDialog.h>
#include <functional>
#include <utility>

namespace NovaTerm::Recording { class HistoryService; }
class HistoryCaptureModel;
class QTableView;
class QLabel;
class ElaPushButton;
class ElaSpinBox;
class HistoryRecordingPanel final : public ElaDialog {
    Q_OBJECT
public:
    explicit HistoryRecordingPanel(NovaTerm::Recording::HistoryService*, QWidget* parent = nullptr);
    void setRecordingTarget(NovaTerm::Recording::RecordingIntent, bool connected, const QString& title);
    void setTargetResolver(std::function<NovaTerm::Recording::RecordingIntent()> resolver) { _resolveTarget = std::move(resolver); }
private:
    HistoryCaptureModel* _model;
    QTableView* _table;
    QLabel* _status;
    QLabel* _targetLabel;
    ElaPushButton* _start;
    ElaPushButton* _stop;
    ElaPushButton* _remove;
    ElaPushButton* _keep;
    ElaSpinBox* _global;
    ElaSpinBox* _capture;
    ElaSpinBox* _days;
    ElaPushButton* _apply;
    NovaTerm::Recording::RecordingIntent _target;
    bool _connected{false};
    bool _settingsEdited{false};
    std::function<NovaTerm::Recording::RecordingIntent()> _resolveTarget;
    int selectedRow() const;
    void updateActions();
    void syncSettings();
};
