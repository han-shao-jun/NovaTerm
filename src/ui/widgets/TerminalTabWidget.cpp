/**
 * @file   TerminalTabWidget.cpp
 * @brief  终端页面专用标签控件实现。
 */
#include "TerminalTabWidget.h"

#include <ElaMenu.h>
#include <ElaTabBar.h>

#include <QAction>
#include <QPointer>

TerminalTabWidget::TerminalTabWidget(QWidget* parent)
    : ElaTabWidget(parent)
{
    auto* const tabs = elaTabBar();
    tabs->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tabs, &QWidget::customContextMenuRequested,
            this, &TerminalTabWidget::showTabContextMenu);
}

ElaTabBar* TerminalTabWidget::elaTabBar() const noexcept
{
    // ElaTabWidget 构造函数无条件安装 ElaTabBar，静态转换保留准确类型。
    return static_cast<ElaTabBar*>(tabBar());
}

void TerminalTabWidget::showTabContextMenu(const QPoint& position)
{
    auto* const tabs = elaTabBar();
    const int index = tabs->tabAt(position);
    if (index < 0 || index >= count())
        return;

    // 与历史会话面板保持一致：右键操作的标签同时成为当前标签，避免菜单目标
    // 与页面当前内容不一致。
    setCurrentIndex(index);

    auto* menu = new ElaMenu(tabs);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setMenuItemHeight(27);
    QAction* const editAction = menu->addElaIconAction(
        ElaIconType::PenToSquare, tr("Edit"));
    const QPointer<QWidget> target = widget(index);
    connect(editAction, &QAction::triggered, this,
            [this, target]() {
        if (!target)
            return;
        const int currentIndex = indexOf(target);
        if (currentIndex >= 0)
            emit editSessionRequested(currentIndex);
    });
    menu->popup(tabs->mapToGlobal(position));
}
