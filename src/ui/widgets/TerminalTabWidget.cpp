/**
 * @file   TerminalTabWidget.cpp
 * @brief  终端页面专用标签控件实现。
 */
#include "TerminalTabWidget.h"

#include <ElaMenu.h>
#include <ElaTabBar.h>

#include <QAction>
#include <QCursor>
#include <QEvent>
#include <QMouseEvent>
#include <QPointer>

TerminalTabWidget::TerminalTabWidget(QWidget* parent)
    : ElaTabWidget(parent)
{
    auto* const tabs = elaTabBar();
    tabs->setMouseTracking(true);
    tabs->installEventFilter(this);
    tabs->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tabs, &QWidget::customContextMenuRequested,
            this, &TerminalTabWidget::showTabContextMenu);

    _hoverSwitchTimer.setSingleShot(true);
    _hoverSwitchTimer.setInterval(HoverSwitchDelayMs);
    connect(&_hoverSwitchTimer, &QTimer::timeout, this, [this]() {
        const int pendingIndex = _pendingHoverIndex;
        _pendingHoverIndex = -1;
        if (pendingIndex < 0 || pendingIndex >= count())
            return;

        auto* const tabs = elaTabBar();
        const QPoint position = tabs->mapFromGlobal(QCursor::pos());
        if (tabTitleAt(position) == pendingIndex
            && currentIndex() != pendingIndex) {
            setCurrentIndex(pendingIndex);
        }
    });
}

ElaTabBar* TerminalTabWidget::elaTabBar() const noexcept
{
    // ElaTabWidget 构造函数无条件安装 ElaTabBar，静态转换保留准确类型。
    return static_cast<ElaTabBar*>(tabBar());
}

int TerminalTabWidget::tabTitleAt(const QPoint& position) const
{
    auto* const tabs = elaTabBar();
    const int index = tabs->tabAt(position);
    if (index < 0 || index >= count() || !tabs->isTabEnabled(index))
        return -1;

    for (const auto side : {QTabBar::LeftSide, QTabBar::RightSide}) {
        QWidget* const button = tabs->tabButton(index, side);
        if (!button || !button->isVisible())
            continue;
        const QRect buttonRect(button->mapTo(tabs, QPoint{}), button->size());
        if (buttonRect.contains(position))
            return -1;
    }
    return index;
}

void TerminalTabWidget::scheduleHoverSwitch(const QPoint& position)
{
    const int index = tabTitleAt(position);
    if (index < 0 || index == currentIndex()) {
        cancelHoverSwitch();
        return;
    }
    if (_pendingHoverIndex == index && _hoverSwitchTimer.isActive())
        return;

    _pendingHoverIndex = index;
    _hoverSwitchTimer.start();
}

void TerminalTabWidget::cancelHoverSwitch()
{
    _hoverSwitchTimer.stop();
    _pendingHoverIndex = -1;
}

bool TerminalTabWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != elaTabBar())
        return ElaTabWidget::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::Enter:
        scheduleHoverSwitch(
            elaTabBar()->mapFromGlobal(QCursor::pos()));
        break;
    case QEvent::MouseMove: {
        const auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->buttons() == Qt::NoButton)
            scheduleHoverSwitch(mouseEvent->position().toPoint());
        else
            cancelHoverSwitch();
        break;
    }
    case QEvent::Leave:
    case QEvent::Hide:
    case QEvent::MouseButtonPress:
        cancelHoverSwitch();
        break;
    default:
        break;
    }
    return ElaTabWidget::eventFilter(watched, event);
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
