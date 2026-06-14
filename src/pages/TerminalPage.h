#pragma once
#include "ElaScrollPage.h"

class TerminalView;
class ElaTabWidget;

// 承载终端的导航页面。使用 ElaTabWidget 管理多个终端标签页，
// 每个标签页内含一个 TerminalView；构造时自动启动第一个本地终端。
class TerminalPage : public ElaScrollPage
{
    Q_OBJECT
public:
    explicit TerminalPage(QWidget* parent = nullptr);
    ~TerminalPage() override;

    // 在内嵌的 TerminalView 中启动（或重启）本地终端。
    void openLocalTerminal();
    TerminalView* currentTerminal() const;

    // 添加一个新的终端标签页
    TerminalView* addTerminalTab(const QString& title = QString());

private:
    void retranslateUi();

    ElaTabWidget* _tabWidget{nullptr};
    QWidget* _centralWidget{nullptr};
    QList<TerminalView*> _terminalViews;
};
