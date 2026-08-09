#pragma once

#include "session/SessionTypes.h"
#include "ui/terminal/TerminalView.h"

#include <QByteArray>
#include <QWidget>
#include <memory>

class CredentialStore;
class ElaIconButton;
class ElaText;
class QTreeWidget;
class QTreeWidgetItem;
class QResizeEvent;
class SessionStore;

class SessionPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit SessionPanel(QWidget* parent = nullptr);
    ~SessionPanel() override;

    void recordLocal(TerminalView::LocalShellType type,
                     const QString& label = {});
    void recordSerial(const SerialConfig& config);
    void recordSsh(const SshConfig& config);
    void updateLocal(const SessionId& id, TerminalView::LocalShellType type,
                     const QString& label);
    void updateSerial(const SessionId& id, const SerialConfig& config);
    void updateSsh(const SessionId& id, const SshConfig& config);
    void setExpanded(bool expanded);
    [[nodiscard]] bool isExpanded() const noexcept { return _expanded; }

protected:
    void resizeEvent(QResizeEvent* event) override;

signals:
    void localReconnectRequested(TerminalView::LocalShellType type);
    void serialReconnectRequested(const SerialConfig& config);
    void sshReconnectRequested(const SshConfig& config);
    void editSessionRequested(const SessionId& id,
                              const RuntimeConfig& runtime,
                              const QByteArray& secret);
    void reconnectUnavailable(const QString& message);

private:
    void repositionToggleButton();
    void retranslateUi();
    void rebuildTree();
    void showItemContextMenu(const QPoint& position);
    void editItem(QTreeWidgetItem* item);
    void deleteItem(QTreeWidgetItem* item);
    void reconnectItem(QTreeWidgetItem* item);
    void upsert(RuntimeConfig runtime, const QByteArray& secret = {});
    void replace(const SessionId& id, RuntimeConfig runtime,
                 const QByteArray& secret = {});
    void saveHistory();
    [[nodiscard]] QString runtimeKey(const RuntimeConfig& runtime) const;

    ElaText* _title{nullptr};
    ElaIconButton* _toggleButton{nullptr};
    QTreeWidget* _tree{nullptr};
    bool _expanded{true};
    QList<SessionRestoreMetadata> _entries;
    std::unique_ptr<SessionStore> _store;
    std::unique_ptr<CredentialStore> _credentials;
};
