/**
 * @file   TabActionWidget.cpp
 * @brief  ElaTabWidget 扩展实现：标签栏尾部动作控件布局。
 *
 * 在 tabInserted/tabRemoved 与 resizeEvent 时调度布局，将动作控件放置在
 * 最后一个标签之后，并在标签栏溢出时为其预留空间。
 */
#include "TabActionWidget.h"

#include <QEvent>
#include <QTabBar>
#include <QTimer>

TabActionWidget::TabActionWidget(QWidget* parent)
    : ElaTabWidget(parent)
{
    tabBar()->installEventFilter(this);
    connect(tabBar(), &QTabBar::tabMoved, this,
            [this]() { scheduleActionWidgetLayout(); });
}

void TabActionWidget::setTabBarActionWidget(QWidget* widget)
{
    _actionWidget = widget;
    _actionWidget->setParent(this);

    // QTabWidget 会将空标签栏折叠为零高度。保留一行正常标签行高度，
    // 使动作控件在首个会话创建前保持垂直居中。
    const int tabBarHeight = qMax(getTabSize().height(),
                                  _actionWidget->height());
    tabBar()->setMinimumHeight(tabBarHeight);

    // 标签溢出且 Qt 显示滚动按钮时仍保持动作控件可达。
    // 有空间时可见控件始终跟随最后一个标签。
    auto* cornerSpacer = new QWidget(this);
    cornerSpacer->setFixedSize(_actionWidget->width() + 8,
                               tabBarHeight);
    setCornerWidget(cornerSpacer, Qt::TopRightCorner);

    _actionWidget->show();
    scheduleActionWidgetLayout();
}

void TabActionWidget::resizeEvent(QResizeEvent* event)
{
    ElaTabWidget::resizeEvent(event);
    scheduleActionWidgetLayout();
}

void TabActionWidget::tabInserted(int index)
{
    ElaTabWidget::tabInserted(index);
    scheduleActionWidgetLayout();
}

void TabActionWidget::tabRemoved(int index)
{
    ElaTabWidget::tabRemoved(index);
    scheduleActionWidgetLayout();
}

bool TabActionWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == tabBar()
        && (event->type() == QEvent::LayoutRequest
            || event->type() == QEvent::Resize
            || event->type() == QEvent::Show)) {
        scheduleActionWidgetLayout();
    }
    return ElaTabWidget::eventFilter(watched, event);
}

void TabActionWidget::scheduleActionWidgetLayout()
{
    if (!_actionWidget || _layoutPending)
        return;

    _layoutPending = true;
    QTimer::singleShot(0, this, [this]() {
        _layoutPending = false;
        updateActionWidgetGeometry();
    });
}

void TabActionWidget::updateActionWidgetGeometry()
{
    if (!_actionWidget)
        return;

    constexpr int spacing = 4;
    const QRect barGeometry = tabBar()->geometry();
    int x = barGeometry.left() + spacing;
    if (count() > 0) {
        const QRect lastTab = tabBar()->tabRect(count() - 1);
        x = barGeometry.left() + lastTab.right() + 1 + spacing;
    }

    const int maximumX = qMax(barGeometry.left(),
                              width() - _actionWidget->width() - spacing);
    x = qBound(barGeometry.left(), x, maximumX);
    const int y = barGeometry.top()
        + (barGeometry.height() - _actionWidget->height()) / 2;
    _actionWidget->move(x, y);
    _actionWidget->raise();
}
