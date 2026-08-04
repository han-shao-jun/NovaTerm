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

    // 在内嵌的 TerminalView 中启动（或重启）本地终端。
    void openLocalTerminal();
    TerminalView* currentTerminal() const;

    // 添加一个新的终端标签页。type 指定本地 Shell 类型（cmd 关联 Clink /
    // PowerShell），转发给内嵌 TerminalView 的 startLocalShell()。
    TerminalView* addTerminalTab(
        const QString& title = QString(),
        TerminalView::LocalShellType type = TerminalView::LocalShellType::Cmd);
    TerminalView* addSerialTerminalTab(const SerialConfig& config);

private:
    void retranslateUi();

    ElaTabWidget* _tabWidget{nullptr};
    QList<TerminalView*> _terminalViews;
};
