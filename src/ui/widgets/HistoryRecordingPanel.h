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
class ElaComboBox;
class ElaCheckBox;
class QLineEdit;
struct HistorySharingActions {
    std::vector<std::pair<QString, QString>> clients; // Stable ID and display label.
    std::function<QString(QString, NovaTerm::Recording::HistoryCaptureId, quint64, quint64, bool)> share;
    std::function<QString(QString, NovaTerm::Recording::HistoryCaptureId)> revoke;
    std::function<QString(QString, NovaTerm::Recording::HistoryCaptureId)> status;
    std::function<void(QString, QString, std::function<void(QString)>)> importFile;
    std::function<void(QString)> clearImports;
};
class HistoryRecordingPanel final : public ElaDialog {
    Q_OBJECT
public:
    explicit HistoryRecordingPanel(NovaTerm::Recording::HistoryService*, QWidget* parent = nullptr);
    void setRecordingTarget(NovaTerm::Recording::RecordingIntent, bool connected, const QString& title);
    void setSharingActions(HistorySharingActions);
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
    QWidget* _sharingPanel;
    ElaComboBox* _shareClient;
    QLineEdit* _shareFirst;
    QLineEdit* _shareLast;
    ElaCheckBox* _sharePersistent;
    ElaPushButton* _share;
    ElaPushButton* _unshare;
    QLabel* _shareStatus;
    ElaPushButton* _importFile;
    ElaPushButton* _clearImports;
    HistorySharingActions _sharing;
    NovaTerm::Recording::RecordingIntent _target;
    bool _connected{false};
    bool _settingsEdited{false};
    std::function<NovaTerm::Recording::RecordingIntent()> _resolveTarget;
    int selectedRow() const;
    void updateActions();
    void syncSettings();
};
