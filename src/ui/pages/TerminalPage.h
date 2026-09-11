/**
 * @file   TerminalPage.h
 * @brief  承载终端的导航页面。
 *
 * 中心直接是一个铺满的 TerminalTabWidget（不经过 ElaScrollPage 的滚动包装），
 * 用于管理多个终端标签页和标签上下文菜单，每个标签页内含一个 TerminalView；
 * 构造时自动启动第一个本地终端。
 */
#pragma once

#include "ui/terminal/TerminalView.h"
#include "session/SessionTypes.h"
#include <QHash>
#include <QWidget>
#include <functional>

class SshTransport;
class TerminalTabWidget;

// 承载终端的导航页面。中心直接就是一个铺满的 TerminalTabWidget（不经过
// ElaScrollPage 的滚动包装），用于管理多个终端标签页，每个标签页内含
// 一个 TerminalView；构造时自动启动第一个本地终端。
class TerminalPage : public QWidget
{
    Q_OBJECT
public:
    explicit TerminalPage(QWidget* parent = nullptr);
    ~TerminalPage() override;

    /**
     * @brief 在内嵌的 TerminalView 中启动（或重启）本地终端。
     */
    void openLocalTerminal();

    /** @brief 当前标签页的终端视图。 */
    TerminalView* currentTerminal() const;

    /**
     * @brief 添加一个新的终端标签页。
     * @param title 标签页标题。
     * @param type  本地 Shell 类型（cmd 关联 Clink / PowerShell / WSL）。
     * @param wslDistribution WSL 发行版名称；其他类型忽略。
     * @return 新建的 TerminalView。
     */
    TerminalView* addTerminalTab(
        const QString& title = QString(),
        TerminalView::LocalShellType type = TerminalView::LocalShellType::Cmd,
        const QString& wslDistribution = {},
        const QString& workingDirectory = {});
    TerminalView* addSerialTerminalTab(const SerialConfig& config);
    TerminalView* addSshTerminalTab(const SshConfig& config);
    TerminalView* addTelnetTerminalTab(const TelnetConfig& config);

    /** 用新参数重建指定运行标签；确认编辑后由 MainWindow 调用。 */
    bool replaceTerminalTab(TerminalView* terminalView,
                            TerminalView::LocalShellType type,
                            const QString& wslDistribution,
                            const QString& label,
                            const QString& workingDirectory);
    bool replaceTerminalTab(TerminalView* terminalView,
                            const SerialConfig& config);
    bool replaceTerminalTab(TerminalView* terminalView,
                            const SshConfig& config);
    bool replaceTerminalTab(TerminalView* terminalView,
                            const TelnetConfig& config);

    /** 将 SFTP 当前目录粘贴到已连接的当前 SSH 终端。 */
    void pastePathToCurrentSshTerminal(const QString& path);
    /** 将已连接的当前 SSH 终端切换到 SFTP 当前目录。 */
    void synchronizeCurrentSshTerminalPath(const QString& path);
    /** 请求用当前 SSH 终端目录切换 SFTP 面板路径。 */
    void requestCurrentSshTerminalPath();

signals:
    void localSessionConnected(TerminalView::LocalShellType type);
    void serialSessionConnected(const SerialConfig& config);
    void sshSessionConnected(const SshConfig& config);
    void telnetSessionConnected(const TelnetConfig& config);
    /** 当前标签或 SSH 连接状态变化，供远端工具面板更新可用性。 */
    void currentSessionContextChanged(const QString& label,
                                      bool connectedSshSession);
    /** 当前已连接 SSH 传输，专供 SFTP 面板建立辅助文件通道。 */
    void currentSftpContextChanged(const QString& label,
                                   SshTransport* transport);
    /** 当前 SSH Shell 工作目录查询完成，供 SFTP 面板反向同步。 */
    void currentSshTerminalPathResolved(const QString& path);
    void currentSshTerminalPathLookupFailed();
    /** 与历史条目“编辑”一致：请求用当前参数打开会话编辑对话框。 */
    void editSessionRequested(TerminalView* terminalView,
                              const RuntimeConfig& runtime,
                              const QByteArray& secret);

private:
    struct SessionEditSnapshot
    {
        RuntimeConfig runtime;
        QByteArray secret;
    };

    void retranslateUi();
    void emitCurrentSessionContext();
    [[nodiscard]] TerminalView* currentConnectedSshTerminal() const;
    void registerTerminalView(TerminalView* terminalView);
    void restartTerminalWhenClosed(TerminalView* terminalView,
                                   std::function<void()> restart);

    TerminalTabWidget* _tabWidget{nullptr};
    QList<TerminalView*> _terminalViews;
    QHash<TerminalView*, SessionEditSnapshot> _sessionEditSnapshots;
};
