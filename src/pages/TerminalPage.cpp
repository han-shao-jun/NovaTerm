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
    auto* terminalView = new TerminalView(_tabWidget);
    _terminalViews.append(terminalView);

    // 关闭标签页时 ElaTabWidget 会 deleteLater() 掉对应的 TerminalView，
    // 但它不知道我们这份 _terminalViews 列表。若不同步移除，列表里会残留
    // 悬垂指针：currentTerminal() 的回退分支 _terminalViews.first() 以及任何
    // 遍历都会访问已释放对象，是切换主题/新建终端时偶发崩溃的根因。
    // 绑定 destroyed 信号确保无论以何种方式销毁（关闭、拖出、父对象析构）
    // 都能把指针从列表里摘掉。
    connect(terminalView, &QObject::destroyed, this, [this](QObject* obj) {
        _terminalViews.removeAll(static_cast<TerminalView*>(obj));
    });

    QString tabTitle = title.isEmpty() ? tr("Terminal %1").arg(_terminalViews.size()) : title;
    int index = _tabWidget->addTab(terminalView, tabTitle);
    _tabWidget->setCurrentIndex(index);

    terminalView->startLocalShell();
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
