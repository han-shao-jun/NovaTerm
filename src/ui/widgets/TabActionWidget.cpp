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

    // QTabWidget collapses an empty tab bar to zero height. Preserve one
    // normal tab row so the action remains vertically centered before the
    // first session is created.
    const int tabBarHeight = qMax(getTabSize().height(),
                                  _actionWidget->height());
    tabBar()->setMinimumHeight(tabBarHeight);

    // Keep the action reachable when tabs overflow and Qt shows its scroll
    // buttons. The visible widget still follows the last tab while space is
    // available.
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
