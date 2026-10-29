#pragma once
#include "ElaDialog.h"
#include "mcp/contracts/SessionBinding.h"
#include "mcp/application/SerialFileTargets.h"
#include <QMap>
#include <QPointer>
#include <functional>
namespace NovaTerm::Mcp { class Service; }
class ElaComboBox;
class ElaLineEdit;
class ElaCheckBox;
class ElaPushButton;
class ElaTreeWidget;
class ElaText;
class RemoteDirectoryDialog;

class FileResourceSharingDialog final : public ElaDialog {
    Q_OBJECT
public:
    FileResourceSharingDialog(NovaTerm::Mcp::Service*, QString client, QWidget* parent = nullptr);
private:
    bool currentIdentity() const;
    void refresh();
    void share();
    void browseRemoteDirectory();
    QPointer<NovaTerm::Mcp::Service> _service;
    QString _client;
    quint64 _identity{0};
    QMap<QString, NovaTerm::Mcp::SessionBinding> _bindings;
    QMap<QString, NovaTerm::Mcp::SerialFileBinding> _serialBindings;
    ElaComboBox* _kind;
    ElaComboBox* _session;
    ElaLineEdit* _path;
    ElaLineEdit* _title;
    ElaCheckBox* _read;
    ElaCheckBox* _write;
    ElaPushButton* _browse;
    ElaPushButton* _share;
    ElaTreeWidget* _roots;
    ElaText* _status;
    bool _refreshing{false};
    quint64 _pathSelectionRevision{0};
    QPointer<RemoteDirectoryDialog> _remoteDirectoryDialog;
    std::function<bool()> _remoteContextIsCurrent;
};
