/**
 * @file   TerminalPage.h
 * @brief  承载终端的导航页面。
 *
 * 中心直接是一个铺满的 ElaTabWidget（不经过 ElaScrollPage 的滚动包装），
 * 用于管理多个终端标签页，每个标签页内含一个 TerminalView；
 * 构造时自动启动第一个本地终端。
 */
#pragma once

#include "ui/terminal/TerminalView.h"
#include "session/SessionTypes.h"
#include <ElaTabWidget.h>
#include <QWidget>

// 承载终端的导航页面。中心直接就是一个铺满的 ElaTabWidget（不经过
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
     * @param type  本地 Shell 类型（cmd 关联 Clink / PowerShell）。
     * @return 新建的 TerminalView。
     */
    TerminalView* addTerminalTab(
        const QString& title = QString(),
        TerminalView::LocalShellType type = TerminalView::LocalShellType::Cmd);
    TerminalView* addSerialTerminalTab(const SerialConfig& config);
    TerminalView* addSshTerminalTab(const SshConfig& config);

signals:
    void localSessionConnected(TerminalView::LocalShellType type);
    void serialSessionConnected(const SerialConfig& config);
    void sshSessionConnected(const SshConfig& config);
    /** 当前标签或 SSH 连接状态变化，供远端工具面板更新可用性。 */
    void currentSessionContextChanged(const QString& label,
                                      bool connectedSshSession);

private:
    void retranslateUi();
    void emitCurrentSessionContext();

    ElaTabWidget* _tabWidget{nullptr};
    QList<TerminalView*> _terminalViews;
};
