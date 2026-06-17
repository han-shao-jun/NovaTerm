#include "TerminalPage.h"
#include "ElaTabWidget.h"
#include "terminal/TerminalView.h"
#include "core/LanguageManager.h"
#include <QVBoxLayout>

TerminalPage::TerminalPage(QWidget* parent) : QWidget(parent)
{
    setWindowTitle(tr("Terminal"));

    _tabWidget = new ElaTabWidget(this);
    _tabWidget->setTabPosition(QTabWidget::North);
    _tabWidget->setIndicatorPosition(ElaTabBarType::Bottom);
    _tabWidget->setTabsClosable(true);
    _tabWidget->setMovable(true);
    _tabWidget->setIsTabTransparent(true);

    // 中心就是纯粹的 ElaTabWidget：零边距、零间距的布局让其完全铺满，
    // 四周不留空隙。标题栏由 ElaWindow 的 AppBar 单独绘制，不受影响。
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(_tabWidget);

    // 启动第一个本地终端标签页
    TerminalView* terminal = addTerminalTab(tr("Terminal"));
    terminal->startLocalShell();

    // 动态语言切换
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });
}

void TerminalPage::retranslateUi()
{
    setWindowTitle(tr("Terminal"));
}

TerminalPage::~TerminalPage() = default;

TerminalView* TerminalPage::currentTerminal() const
{
    QWidget* currentWidget = _tabWidget->currentWidget();
    if (currentWidget)
    {
        return currentWidget->findChild<TerminalView*>();
    }
    return _terminalViews.isEmpty() ? nullptr : _terminalViews.first();
}

TerminalView* TerminalPage::addTerminalTab(const QString& title)
{
    TerminalView* terminalView = new TerminalView(_tabWidget);
    _terminalViews.append(terminalView);

    QString tabTitle = title.isEmpty() ? tr("Terminal %1").arg(_terminalViews.size()) : title;
    int index = _tabWidget->addTab(terminalView, tabTitle);
    _tabWidget->setCurrentIndex(index);

    return terminalView;
}

void TerminalPage::openLocalTerminal()
{
    // QTermWidget 内置 KPty 直接启动 shell → 零额外 Transport 依赖
    // 交互程序 (vim/htop/tmux) 完美工作 — 因为走的是真实的 ConPTY/pty
    TerminalView* terminal = currentTerminal();
    if (terminal)
    {
        terminal->startLocalShell();
    }
}
