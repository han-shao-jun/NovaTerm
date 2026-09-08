#include "ElaTabWidgetPrivate.h"

#include "ElaCustomTabWidget.h"
#include "ElaTabBar.h"
#include "ElaTabWidget.h"
#include <QApplication>
#include <QDebug>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QTimer>
#include <QWindow>
ElaDragMonitor::ElaDragMonitor(QObject* parent)
    : QObject(parent)
{
    _pIsInDrag = false;
}

ElaDragMonitor::~ElaDragMonitor()
{
}

ElaTabWidgetPrivate::ElaTabWidgetPrivate(QObject* parent)
    : QObject{parent}
{
}

ElaTabWidgetPrivate::~ElaTabWidgetPrivate()
{
}

void ElaTabWidgetPrivate::onTabDragCreate(QMimeData* mimeData)
{
    Q_Q(ElaTabWidget);
    if (ElaDragMonitor::getInstance()->getIsInDrag())
    {
        return;
    }
    ElaDragMonitor::getInstance()->setIsInDrag(true);
    mimeData->setProperty("ElaTabWidgetObject", QVariant::fromValue(q));
    int index = q->currentIndex();
    QString tabText = q->tabText(index);
    QIcon tabIcon = q->tabIcon(index);
    QWidget* dragWidget = q->widget(index);
    dragWidget->setProperty("TabIcon", q->tabIcon(index));
    dragWidget->setProperty("TabText", q->tabText(index));
    QWidget* originTabWidget = dragWidget->property("ElaOriginTabWidget").value<ElaTabWidget*>();
    if (!originTabWidget && !dragWidget->property("IsMetaWidget").toBool())
    {
        dragWidget->setProperty("ElaOriginTabWidget", QVariant::fromValue<ElaTabWidget*>(q));
        originTabWidget = q;
    }
    mimeData->setProperty("DragWidget", QVariant::fromValue(dragWidget));
    ElaTabBar* tabBarObject = mimeData->property("ElaTabBarObject").value<ElaTabBar*>();
    QMouseEvent releaseEvent(QEvent::MouseButtonRelease, QPoint(-1, -1), QPoint(-1, -1), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tabBarObject, &releaseEvent);
    if (!originTabWidget)
    {
        originTabWidget = dragWidget->property("ElaFloatParentWidget").value<QWidget*>();
    }
    bool isFloatWidget = mimeData->property("IsFloatWidget").toBool();
    QSize tabSize = mimeData->property("TabSize").toSize();
    ElaCustomTabWidget* floatWidget = nullptr;
    if (isFloatWidget)
    {
        floatWidget = dynamic_cast<ElaCustomTabWidget*>(q->window());
        if (!floatWidget)
        {
            ElaDragMonitor::getInstance()->setIsInDrag(false);
            return;
        }
        floatWidget->setDragInputTransparent(true);
    }
    else
    {
        floatWidget = new ElaCustomTabWidget(originTabWidget);
        q->removeTab(index);
        // 创建新窗口
        ElaTabBar* originCustomTabBar = tabBarObject;
        if (originCustomTabBar && originCustomTabBar->objectName() == "ElaCustomTabBar")
        {
            originCustomTabBar->removeTab(index);
        }
        ElaTabBar* customTabBar = floatWidget->getCustomTabBar();
        customTabBar->setTabSize(tabSize);
        dragWidget->setProperty("CurrentCustomBar", QVariant::fromValue<ElaTabBar*>(customTabBar));
        floatWidget->addTab(dragWidget, tabIcon, tabText);
    }
    QPoint dragPos = mimeData->property("DragPos").toPoint();
    // floatWidget 可能在拖拽过程中被销毁（例如标签中途停靠回原 TabWidget），
    // 回调统一以 QPointer 守卫；接收者上下文改为 this，保证浮窗销毁后回调仍
    // 能执行到停表逻辑，否则定时器会成为无人清理的孤儿。
    QPointer<ElaCustomTabWidget> floatGuard(floatWidget);
    QTimer* dragTimer = new QTimer(this);
    dragTimer->start(10);
    // IsFinished 只在浮窗标签清空时置位，不能作为唯一停止条件：拖放结束后若
    // 浮窗仍持有标签，此定时器会一直把窗口拉回光标处，表现为浮窗无法最大化、
    // 标题栏按钮无法点击。拖拽结束（ElaDragMonitor 复位）时必须立即停表。
    connect(dragTimer, &QTimer::timeout, this, [=]() {
        if (!ElaDragMonitor::getInstance()->getIsInDrag()
            || (floatGuard && floatGuard->getIsFinished() && !isFloatWidget))
        {
            dragTimer->stop();
            dragTimer->deleteLater();
            return;
        }
        if (!floatGuard)
        {
            return;
        }
        QPoint cursorPoint = QCursor::pos();
        if (isFloatWidget)
        {
            floatGuard->move(cursorPoint.x() - dragPos.x() - 10, cursorPoint.y() - dragPos.y() - 10);
        }
        else
        {
            floatGuard->move(cursorPoint.x() - tabSize.width() / 2 - 10, cursorPoint.y() - tabSize.height() / 2 - 10);
        }
    });
    QDrag* drag = new QDrag(this);
    QPixmap pix(1, 1);
    pix.fill(Qt::transparent);
    drag->setPixmap(pix);
    drag->setMimeData(mimeData);
    drag->setHotSpot(QPoint(tabSize.width() / 2, 0));
    QTimer::singleShot(1, this, [=]() {
        floatWidget->show();
        floatWidget->setDragInputTransparent(true);
        if (!isFloatWidget)
        {
            floatWidget->resize(700, 500);
        }
    });
    auto ret = drag->exec();
    ElaDragMonitor::getInstance()->setIsInDrag(false);
    ElaCustomTabWidget* tempFloatWidget = mimeData->property("TempFloatWidget").value<ElaCustomTabWidget*>();
    if (tempFloatWidget)
    {
        if (ret == Qt::IgnoreAction)
        {
            tempFloatWidget->setDragInputTransparent(false);
        }
        else
        {
            tempFloatWidget->deleteLater();
        }
        floatWidget->deleteLater();
    }
    else if (floatWidget->isVisible())
    {
        floatWidget->setDragInputTransparent(false);
    }
    else
    {
        // 标签已停靠回原 TabWidget 时浮窗会自行隐藏，直接销毁空浮窗，避免
        // setDragInputTransparent 延迟回调中的 show() 把空窗口重新拉起。
        floatWidget->deleteLater();
    }
}

void ElaTabWidgetPrivate::onTabDragEnter(QMimeData* mimeData)
{
    Q_Q(ElaTabWidget);
    mimeData->setProperty("ElaTabBarObject", QVariant::fromValue<ElaTabBar*>(dynamic_cast<ElaTabBar*>(q->tabBar())));
    onTabDragDrop(mimeData);
}

void ElaTabWidgetPrivate::onTabDragLeave(QMimeData* mimeData)
{
    Q_Q(ElaTabWidget);
    QWidget* dragWidget = mimeData->property("DragWidget").value<QWidget*>();
    int index = q->indexOf(dragWidget);
    mimeData->setProperty("ElaTabWidgetObject", QVariant::fromValue(q));
    QString tabText = q->tabText(index);
    QIcon tabIcon = q->tabIcon(index);

    ElaTabBar* tabBarObject = mimeData->property("ElaTabBarObject").value<ElaTabBar*>();
    mimeData->setProperty("DragWidget", QVariant::fromValue(dragWidget));
    q->removeTab(index);
    // 创建新窗口
    if (_customTabBar && _customTabBar != tabBarObject)
    {
        _customTabBar->removeTab(index);
    }
    QWidget* originTabWidget = dragWidget->property("ElaOriginTabWidget").value<ElaTabWidget*>();
    if (!originTabWidget)
    {
        originTabWidget = dragWidget->property("ElaFloatParentWidget").value<QWidget*>();
    }
    ElaCustomTabWidget* floatWidget = new ElaCustomTabWidget(originTabWidget);
    QSize tabSize = mimeData->property("TabSize").toSize();
    ElaTabBar* customTabBar = floatWidget->getCustomTabBar();
    customTabBar->setTabSize(tabSize);
    dragWidget->setProperty("CurrentCustomBar", QVariant::fromValue<ElaTabBar*>(customTabBar));
    floatWidget->addTab(dragWidget, tabIcon, tabText);
    floatWidget->show();
    floatWidget->resize(700, 500);
    floatWidget->setDragInputTransparent(true);
    QPoint cursorPoint = QCursor::pos();
    floatWidget->move(cursorPoint.x() - tabSize.width() / 2, cursorPoint.y() - tabSize.height() / 2);
    // 与 onTabDragCreate 相同：拖拽结束即停表，防止临时浮窗持续追随光标；
    // 旧实现依赖 mimeData 析构清理定时器，而 QDrag 的生命周期远长于拖拽本身。
    QPointer<ElaCustomTabWidget> tempGuard(floatWidget);
    QTimer* dragTimer = new QTimer(this);
    dragTimer->start(10);
    connect(dragTimer, &QTimer::timeout, this, [=]() {
        if (!ElaDragMonitor::getInstance()->getIsInDrag())
        {
            dragTimer->stop();
            dragTimer->deleteLater();
            return;
        }
        if (!tempGuard)
        {
            return;
        }
        QPoint cursorPoint = QCursor::pos();
        tempGuard->move(cursorPoint.x() - tabSize.width() / 2 - 10, cursorPoint.y() - tabSize.height() / 2 - 10);
    });
    ElaCustomTabWidget* tempFloatWidget = mimeData->property("TempFloatWidget").value<ElaCustomTabWidget*>();
    if (tempFloatWidget)
    {
        tempFloatWidget->deleteLater();
    }
    mimeData->setProperty("TempFloatWidget", QVariant::fromValue<ElaCustomTabWidget*>(floatWidget));
}

void ElaTabWidgetPrivate::onTabDragDrop(QMimeData* mimeData)
{
    Q_Q(ElaTabWidget);
    QWidget* dragWidget = mimeData->property("DragWidget").value<QWidget*>();
    QString tabText = dragWidget->property("TabText").toString();
    QIcon tabIcon = dragWidget->property("TabIcon").value<QIcon>();
    int dropIndex = mimeData->property("TabDropIndex").toInt();
    if (dropIndex < 0)
    {
        dropIndex = q->count();
    }
    q->insertTab(dropIndex, dragWidget, tabIcon, tabText);
    q->setCurrentWidget(dragWidget);
    if (_customTabBar)
    {
        dragWidget->setProperty("CurrentCustomBar", QVariant::fromValue<ElaTabBar*>(_customTabBar));
        _customTabBar->insertTab(dropIndex, tabIcon, tabText);
        _customTabBar->setCurrentIndex(dropIndex);
    }
}

void ElaTabWidgetPrivate::onTabCloseRequested(int index)
{
    Q_Q(ElaTabWidget);
    QWidget* closeWidget = q->widget(index);
    ElaTabWidget* originTabWidget = closeWidget->property("ElaOriginTabWidget").value<ElaTabWidget*>();
    if (originTabWidget && originTabWidget != q)
    {
        ElaTabBar* customTabBar = closeWidget->property("CurrentCustomBar").value<ElaTabBar*>();
        if (customTabBar)
        {
            customTabBar->removeTab(index);
            closeWidget->setProperty("CurrentCustomBar", QVariant::fromValue<ElaTabBar*>(nullptr));
        }
        originTabWidget->addTab(closeWidget, q->tabIcon(index), q->tabText(index));
        originTabWidget->setCurrentWidget(closeWidget);
    }
    else
    {
        if (!originTabWidget && q->objectName() == "ElaCustomTabWidget")
        {
            _customTabBar->removeTab(index);
        }
        q->removeTab(index);
        if (_allTabWidgetList.contains(closeWidget))
        {
            _allTabWidgetList.removeOne(closeWidget);
        }
        closeWidget->deleteLater();
    }
}

void ElaTabWidgetPrivate::_clearAllTabWidgetList()
{
    Q_Q(ElaTabWidget);
    for (auto widget: _allTabWidgetList)
    {
        auto originTabWidgetVariant = widget->property("ElaOriginTabWidget");
        if (originTabWidgetVariant.isValid() && originTabWidgetVariant.value<ElaTabWidget*>() == q)
        {
            widget->setProperty("ElaOriginTabWidget", QVariant());
        }
    }
    _allTabWidgetList.clear();
}
