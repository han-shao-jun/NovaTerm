/** @file McpSettingsDialog.h
 *  @brief MCP 接入配置、会话共享与受限命令授权。
 */
#pragma once
#include "ElaDialog.h"
#include <QPointer>
#include <QJsonObject>
#include <QHash>
namespace NovaTerm::Mcp { class Service; }
class ElaCheckBox;
class ElaComboBox;
class ElaLineEdit;
class ElaPlainTextEdit;
class ElaText;
class ElaTreeWidget;

class McpSettingsDialog final : public ElaDialog
{
    Q_OBJECT
public:
    explicit McpSettingsDialog(NovaTerm::Mcp::Service* service, QWidget* parent);
private:
    void refresh();
    void showRecord();
    void reportFailure();
    QPointer<NovaTerm::Mcp::Service> _service;
    ElaCheckBox* _enabled{nullptr};
    ElaComboBox* _clients{nullptr};
    ElaLineEdit* _label{nullptr};
    ElaText* _status{nullptr};
    ElaTreeWidget* _sessions{nullptr};
    ElaComboBox* _records{nullptr};
    ElaPlainTextEdit* _details{nullptr};
    ElaComboBox* _targets{nullptr};
    ElaCheckBox* _confirmedStopped{nullptr};
    QHash<QString, QJsonObject> _recordData;
    bool _refreshing{false};
};
