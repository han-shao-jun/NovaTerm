#pragma once
#include <ElaDialog.h>
#include "session/SftpDirectoryListing.h"
#include <QPointer>
#include <functional>

class ElaLineEdit;
class ElaPushButton;
class ElaText;
class ElaTreeWidget;

/** 只读异步远端目录选择；不持有连接、不创建文件、不授予权限。 */
class RemoteDirectoryDialog final : public ElaDialog {
    Q_OBJECT
public:
    using Completion = std::function<void(SftpDirectoryResult)>;
    using List = std::function<void(const QString&, QObject*, Completion)>;
    RemoteDirectoryDialog(QString initialPath, List, std::function<bool()> current, QWidget* parent = nullptr);
    QString selectedDirectory() const { return _selected; }
public slots:
    void accept() override;
    void reject() override;
private:
    void load(const QString&);
    void setBusy(bool);
    void cancelRequest();
    List _list;
    std::function<bool()> _current;
    QPointer<QObject> _requestContext;
    quint64 _revision{0};
    QString _currentPath, _selected;
    bool _loading{false}, _valid{false};
    ElaLineEdit* _path;
    ElaPushButton* _up;
    ElaPushButton* _home;
    ElaPushButton* _go;
    ElaPushButton* _select;
    ElaTreeWidget* _tree;
    ElaText* _status;
};
