#include "ElaCustomTabWidget.h"

#include "ElaAppBar.h"
#include "ElaTabBar.h"
#include "ElaTabWidget.h"
#include "ElaTabWidgetPrivate.h"
#include <QDebug>
#include <QEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <QWindow>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {

void clearNativeInputTransparency(QWidget* widget)
{
#ifdef Q_OS_WIN
    const HWND window = reinterpret_cast<HWND>(widget->winId());
    const LONG_PTR extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((extendedStyle & WS_EX_TRANSPARENT) == 0)
        return;
    SetWindowLongPtrW(window, GWL_EXSTYLE,
                      extendedStyle
                          & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                     | SWP_NOACTIVATE | SWP_FRAMECHANGED);
#else
    Q_UNUSED(widget);
#endif
}

void applyNativeMaximizedGeometry(QWidget* widget)
{
#ifdef Q_OS_WIN
    const HWND window = reinterpret_cast<HWND>(widget->winId());
    MONITORINFO monitorInfo{sizeof(MONITORINFO)};
    const HMONITOR monitor = MonitorFromWindow(
        window, MONITOR_DEFAULTTONEAREST);
    if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo))
        return;

    const RECT& workRect = monitorInfo.rcWork;
    SetWindowPos(window, nullptr, workRect.left, workRect.top,
                 workRect.right - workRect.left,
                 workRect.bottom - workRect.top,
                 SWP_NOZORDER | SWP_NOOWNERZORDER
                     | SWP_NOACTIVATE | SWP_FRAMECHANGED);
#else
    Q_UNUSED(widget);
#endif
}

} // namespace

ElaCustomTabWidget::ElaCustomTabWidget(QWidget* parent)
    : ElaCustomWidget(parent)
{
    _pIsFinished = false;
    resize(700, 500);
    setWindowTitle("");
#ifndef Q_OS_WIN
    setAttribute(Qt::WA_Hover);
#endif
    setMouseTracking(true);
    setWindowIcon(QIcon());
    _customTabWidget = new ElaTabWidget(this);
    _customTabWidget->setIsTabTransparent(true);
    _customTabWidget->setObjectName("ElaCustomTabWidget");
    QTabBar* originTabBar = _customTabWidget->tabBar();
    originTabBar->hide();
    setAcceptDrops(true);
    _customTabBar = new ElaTabBar(this);
    _customTabBar->setObjectName("ElaCustomTabBar");
    connect(_customTabBar, &ElaTabBar::tabMoved, this, [=](int from, int to) {
        _customTabWidget->tabBar()->moveTab(from, to);
    });
    connect(_customTabBar, &ElaTabBar::currentChanged, this, [=](int index) {
        _customTabWidget->setCurrentIndex(index);
    });
    connect(_customTabWidget, &ElaTabWidget::currentChanged, this, [=](int index) {
        if (index == -1)
        {
            _pIsFinished = true;
            hide();
        }
    });
    connect(_customTabBar, &ElaTabBar::tabCloseRequested, originTabBar, &QTabBar::tabCloseRequested);
    // 浮窗没有任何对象接收 ElaAppBar::closeButtonClicked 信号，点击标题栏关闭
    // 按钮（以及 Alt+F4 触发的自发关闭事件）会毫无反应。这里统一接管：close()
    // 经 WA_DeleteOnClose 触发析构，由析构函数把剩余标签迁回原 TabWidget。
    connect(_appBar, &ElaAppBar::closeButtonClicked, this, &ElaCustomTabWidget::close);

    _customTabWidget->d_ptr->_customTabBar = _customTabBar;
    connect(_customTabBar, &ElaTabBar::tabDragCreate, _customTabWidget->d_func(), &ElaTabWidgetPrivate::onTabDragCreate);
    connect(_customTabBar, &ElaTabBar::tabDragDrop, _customTabWidget->d_func(), &ElaTabWidgetPrivate::onTabDragDrop);
    connect(_customTabBar, &ElaTabBar::tabDragEnter, _customTabWidget->d_func(), &ElaTabWidgetPrivate::onTabDragEnter);
    connect(_customTabBar, &ElaTabBar::tabDragLeave, _customTabWidget->d_func(), &ElaTabWidgetPrivate::onTabDragLeave);
    QWidget* customWidget = new QWidget(this);
    QVBoxLayout* customLayout = new QVBoxLayout(customWidget);
    customLayout->setContentsMargins(10, 0, 10, 0);
    customLayout->addStretch();
    customLayout->addWidget(_customTabBar);
    _appBar->setCustomWidget(ElaAppBarType::LeftArea, customWidget, this, "processHitTest");
    setCentralWidget(_customTabWidget);
}

ElaCustomTabWidget::~ElaCustomTabWidget()
{
    while (_customTabWidget->count() > 0)
    {
        QWidget* closeWidget = _customTabWidget->widget(0);
        ElaTabWidget* originTabWidget = closeWidget->property("ElaOriginTabWidget").value<ElaTabWidget*>();
        if (originTabWidget)
        {
            closeWidget->setProperty("CurrentCustomBar", QVariant::fromValue<ElaTabBar*>(nullptr));
            originTabWidget->addTab(closeWidget, _customTabWidget->tabIcon(0), _customTabWidget->tabText(0));
            originTabWidget->setCurrentWidget(closeWidget);
        }
        else
        {
            _customTabWidget->removeTab(0);
        }
    }
}

void ElaCustomTabWidget::addTab(QWidget* widget, QIcon tabIcon, const QString& tabTitle)
{
    _customTabBar->addTab(tabIcon, tabTitle);
    _customTabWidget->addTab(widget, tabIcon, tabTitle);
}

ElaTabBar* ElaCustomTabWidget::getCustomTabBar() const
{
    return _customTabBar;
}

ElaTabWidget* ElaCustomTabWidget::getCustomTabWidget() const
{
    return _customTabWidget;
}

void ElaCustomTabWidget::setDragInputTransparent(bool transparent)
{
    if (QWindow* const handle = windowHandle())
        handle->setFlag(Qt::WindowTransparentForInput, transparent);
    if (transparent)
        return;
    clearNativeInputTransparency(this);

    // QDrag::exec() 使用嵌套事件循环，期间浮窗可能被替换并重建原生句柄。
    // 当前调用先清一次；回到主事件循环后再对最终句柄清一次，避免 Windows
    // 残留 WS_EX_TRANSPARENT，导致最大化、关闭等标题栏操作全部失效。
    QTimer::singleShot(0, this, [this]() {
        if (QWindow* const handle = windowHandle())
            handle->setFlag(Qt::WindowTransparentForInput, false);
        clearNativeInputTransparency(this);
        // 标签已全部停靠回原 TabWidget 的空浮窗不再重新拉起（此时已走
        // deleteLater 销毁流程），仅当窗口仍持有标签时才恢复显示。
        if (!isVisible() && _customTabWidget->count() > 0)
            show();
        raise();
        activateWindow();
    });
}

bool ElaCustomTabWidget::processHitTest()
{
    auto point = _customTabBar->mapFromGlobal(QCursor::pos());
    return _customTabBar->tabAt(point) < 0;
}

void ElaCustomTabWidget::changeEvent(QEvent* event)
{
    ElaCustomWidget::changeEvent(event);
    if (event->type() != QEvent::WindowStateChange || !isMaximized())
        return;

    // QDialog 有 native owner，Qt 已切换 WindowMaximized 状态后，Windows 偶尔
    // 仍保留拖出时的 700×500 HWND 矩形。回到主事件循环后直接按浮窗所在显示器
    // 的工作区校正最终原生句柄，且只影响 Ela 标签拖出浮窗。
    QTimer::singleShot(0, this, [this]() {
        if (isMaximized())
            applyNativeMaximizedGeometry(this);
    });
}
