#include "ElaTabBar.h"

#include <QApplication>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QPropertyAnimation>

#include "ElaTabBarPrivate.h"
#include "ElaTabBarStyle.h"
#include "private/qtabbar_p.h"
#include <QTimer>
ElaTabBar::ElaTabBar(QWidget* parent)
    : QTabBar(parent), d_ptr(new ElaTabBarPrivate())
{
    Q_D(ElaTabBar);
    d->q_ptr = this;
    setObjectName("ElaTabBar");
    setMouseTracking(true);
    setStyleSheet("#ElaTabBar{background-color:transparent;}");
    setTabsClosable(true);
    setMovable(true);
    setAcceptDrops(true);
    d->_style = new ElaTabBarStyle(style());
    setStyle(d->_style);
    d->_tabBarPrivate = dynamic_cast<QTabBarPrivate*>(this->QTabBar::d_ptr.data());

    // Animation property defaults
    _pIndicatorX = 0;
    _pIndicatorWidth = 40;
    _pIndicatorAnimationWidth = 0;
    _pIndicatorY = 0;
    _pIndicatorHeight = 40;
    _pIndicatorAnimationHeight = 0;
    _pIsIndicatorAnimationFinished = true;

    // Trigger animation on tab change
    connect(this, &QTabBar::currentChanged, this, [=](int index) {
        doIndicatorAnimation(_previousIndex, index);
        _previousIndex = index;
    });
}

ElaTabBar::~ElaTabBar()
{
    Q_D(ElaTabBar);
    delete d->_style;
}

void ElaTabBar::setTabSize(QSize tabSize)
{
    Q_D(ElaTabBar);
    d->_style->setTabSize(tabSize);
}

QSize ElaTabBar::getTabSize() const
{
    Q_D(const ElaTabBar);
    return d->_style->getTabSize();
}

void ElaTabBar::setIndicatorPosition(ElaTabBarType::IndicatorPosition pos)
{
    Q_D(ElaTabBar);
    d->_style->setIndicatorPosition(pos);
    update();
}

ElaTabBarType::IndicatorPosition ElaTabBar::getIndicatorPosition() const
{
    Q_D(const ElaTabBar);
    return d->_style->getIndicatorPosition();
}

void ElaTabBar::setTabPosition(ElaTabBarType::TabPosition pos)
{
    Q_D(ElaTabBar);
    d->_style->setTabPosition(pos);
    update();
}

ElaTabBarType::TabPosition ElaTabBar::getTabPosition() const
{
    Q_D(const ElaTabBar);
    return d->_style->getTabPosition();
}

bool ElaTabBar::isVertical() const
{
    auto pos = getTabPosition();
    return pos == ElaTabBarType::West || pos == ElaTabBarType::East;
}

QSize ElaTabBar::sizeHint() const
{
    QSize oldSize = QTabBar::sizeHint();
    if (expanding())
    {
        return oldSize;
    }
    QSize newSize = oldSize;
    if (isVertical())
    {
        // 垂直模式：沿 bar 方向（高度）填满父控件
        newSize.setHeight(parentWidget()->maximumHeight());
    }
    else
    {
        // 水平模式：沿 bar 方向（宽度）填满父控件
        newSize.setWidth(parentWidget()->maximumWidth());
    }
    return oldSize.expandedTo(newSize);
}

void ElaTabBar::mouseMoveEvent(QMouseEvent* event)
{
    QTabBar::mouseMoveEvent(event);
    Q_D(ElaTabBar);
    if (d->_tabBarPrivate->pressedIndex >= 0)
    {
        QPoint currentPos = event->pos();
        if (objectName() == "ElaCustomTabBar" && count() == 1)
        {
            if (!d->_mimeData)
            {
                d->_mimeData = new QMimeData();
                d->_mimeData->setProperty("DragType", "ElaTabBarDrag");
                d->_mimeData->setProperty("ElaTabBarObject", QVariant::fromValue(this));
                d->_mimeData->setProperty("TabSize", d->_style->getTabSize());
                d->_mimeData->setProperty("IsFloatWidget", true);
                QRect currentTabRect = tabRect(currentIndex());
                d->_mimeData->setProperty("DragPos", QPoint(currentPos.x() - currentTabRect.x(), currentPos.y() - currentTabRect.y()));
                Q_EMIT tabDragCreate(d->_mimeData);
                d->_mimeData = nullptr;
            }
        }
        else
        {
            auto& pressTabData = d->_tabBarPrivate->tabList[d->_tabBarPrivate->pressedIndex];
            QRect firstTabRect = tabRect(0);
#if (QT_VERSION > QT_VERSION_CHECK(6, 0, 0))
            QRect pressTabRect = pressTabData->rect;
            if (pressTabRect.right() + pressTabData->dragOffset > width() - firstTabRect.x())
            {
                pressTabData->dragOffset = width() - pressTabRect.right() - firstTabRect.x();
            }
            if (pressTabRect.x() + pressTabData->dragOffset < -firstTabRect.x())
            {
                pressTabData->dragOffset = -pressTabRect.x() - firstTabRect.x();
            }
#else
            QRect pressTabRect = pressTabData.rect;
            if (pressTabRect.right() + pressTabData.dragOffset > width() - firstTabRect.x())
            {
                pressTabData.dragOffset = width() - pressTabRect.right() - firstTabRect.x();
            }
            if (pressTabRect.x() + pressTabData.dragOffset < -firstTabRect.x())
            {
                pressTabData.dragOffset = -pressTabRect.x() - firstTabRect.x();
            }
#endif

            QRect moveRect = rect();
            if (isVertical())
            {
                moveRect.adjust(-width(), 0, width(), 0);
                if (currentPos.y() < 0 || currentPos.y() > height() || currentPos.x() > moveRect.right() || currentPos.x() < moveRect.x())
                {
                    if (!d->_mimeData)
                    {
                        d->_mimeData = new QMimeData();
                        d->_mimeData->setProperty("DragType", "ElaTabBarDrag");
                        d->_mimeData->setProperty("ElaTabBarObject", QVariant::fromValue(this));
                        d->_mimeData->setProperty("TabSize", d->_style->getTabSize());
                        Q_EMIT tabDragCreate(d->_mimeData);
                        d->_mimeData = nullptr;
                    }
                }
            }
            else
            {
                moveRect.adjust(0, -height(), 0, height());
                if (currentPos.x() < 0 || currentPos.x() > width() || currentPos.y() > moveRect.bottom() || currentPos.y() < moveRect.y())
                {
                    if (!d->_mimeData)
                    {
                        d->_mimeData = new QMimeData();
                        d->_mimeData->setProperty("DragType", "ElaTabBarDrag");
                        d->_mimeData->setProperty("ElaTabBarObject", QVariant::fromValue(this));
                        d->_mimeData->setProperty("TabSize", d->_style->getTabSize());
                        Q_EMIT tabDragCreate(d->_mimeData);
                        d->_mimeData = nullptr;
                    }
                }
            }
        }
    }
}

void ElaTabBar::dragEnterEvent(QDragEnterEvent* event)
{
    Q_D(ElaTabBar);
    if (event->mimeData()->property("DragType").toString() == "ElaTabBarDrag")
    {
        event->acceptProposedAction();
        auto mimeData = const_cast<QMimeData*>(event->mimeData());
        d->_mimeData = mimeData;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        mimeData->setProperty("TabDropIndex", tabAt(event->position().toPoint()));
#else
        mimeData->setProperty("TabDropIndex", tabAt(event->pos()));
#endif
        Q_EMIT tabDragEnter(mimeData);
        qApp->processEvents();
        if (isVertical())
        {
            QMouseEvent pressEvent(QEvent::MouseButtonPress, QPoint(0, tabRect(currentIndex()).y() + d->_style->getTabSize().height() / 2), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(this, &pressEvent);
            QMouseEvent moveEvent(QEvent::MouseMove, QPoint(0, event->pos().y()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(this, &moveEvent);
        }
        else
        {
            QMouseEvent pressEvent(QEvent::MouseButtonPress, QPoint(tabRect(currentIndex()).x() + d->_style->getTabSize().width() / 2, 0), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(this, &pressEvent);
            QMouseEvent moveEvent(QEvent::MouseMove, QPoint(event->pos().x(), 0), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(this, &moveEvent);
        }
    }
    QTabBar::dragEnterEvent(event);
}

void ElaTabBar::dragMoveEvent(QDragMoveEvent* event)
{
    Q_D(ElaTabBar);
    if (event->mimeData()->property("DragType").toString() == "ElaTabBarDrag")
    {
        if (isVertical())
        {
            QMouseEvent moveEvent(QEvent::MouseMove, QPoint(0, event->pos().y()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(this, &moveEvent);
        }
        else
        {
            QMouseEvent moveEvent(QEvent::MouseMove, QPoint(event->pos().x(), 0), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(this, &moveEvent);
        }
    }
    QWidget::dragMoveEvent(event);
}

void ElaTabBar::dragLeaveEvent(QDragLeaveEvent* event)
{
    Q_D(ElaTabBar);
    if (d->_mimeData)
    {
        Q_EMIT tabDragLeave(d->_mimeData);
        d->_mimeData = nullptr;
    }
    QMouseEvent releaseEvent(QEvent::MouseButtonRelease, QPoint(-1, -1), QPoint(-1, -1), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(this, &releaseEvent);
    QTabBar::dragLeaveEvent(event);
}

void ElaTabBar::dropEvent(QDropEvent* event)
{
    Q_D(ElaTabBar);
    d->_mimeData = nullptr;
    QMouseEvent releaseEvent(QEvent::MouseButtonRelease, QPoint(-1, -1), QPoint(-1, -1), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(this, &releaseEvent);
    if (objectName() != "ElaCustomTabBar")
    {
        QMimeData* data = const_cast<QMimeData*>(event->mimeData());
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        data->setProperty("TabDropIndex", tabAt(event->position().toPoint()));
#else
        data->setProperty("TabDropIndex", tabAt(event->pos()));
#endif
        Q_EMIT tabDragDrop(data);
    }
    QTabBar::dropEvent(event);
}

void ElaTabBar::wheelEvent(QWheelEvent* event)
{
    QTabBar::wheelEvent(event);
    event->accept();
}

void ElaTabBar::paintEvent(QPaintEvent* event)
{
    Q_D(ElaTabBar);
    QSize tabSize = d->_style->getTabSize();
    int tabCount = d->_tabBarPrivate->tabList.size();
    if (tabCount > 0) {
        if (isVertical())
        {
            // 垂直模式：交给 Qt 原生布局，不手动修正 rect
        }
        else
        {
            int tabWidth = expanding() ? (width() / tabCount) : tabSize.width();
            for (int i = 0; i < tabCount; i++)
            {
#if (QT_VERSION > QT_VERSION_CHECK(6, 0, 0))
                d->_tabBarPrivate->tabList[i]->rect = QRect(tabWidth * i, d->_tabBarPrivate->tabList[i]->rect.y(), tabWidth, tabSize.height());
#else
                d->_tabBarPrivate->tabList[i].rect = QRect(tabWidth * i, d->_tabBarPrivate->tabList[i].rect.y(), tabWidth, tabSize.height());
#endif
            }
        }
    }
    d->_tabBarPrivate->layoutWidgets();
    QTabBar::paintEvent(event);
}

void ElaTabBar::doIndicatorAnimation(int previousIndex, int currentIndex)
{
    // ── Vertical animation (Right indicator) ────────────
    if (getIndicatorPosition() == ElaTabBarType::Right)
    {
        _previousTabRect = (previousIndex >= 0) ? tabRect(previousIndex) : QRect();
        QRect currentRect = tabRect(currentIndex);
        if (!currentRect.isValid())
        {
            _pIsIndicatorAnimationFinished = true;
            update();
            return;
        }

        const int verticalPadding = 7;
        int targetY = currentRect.top() + verticalPadding;
        int targetHeight = currentRect.height() - 2 * verticalPadding;

        if (previousIndex >= 0 && _previousTabRect.isValid())
        {
            int prevY = _previousTabRect.top() + verticalPadding;
            QPropertyAnimation* anim = new QPropertyAnimation(this, "pIndicatorY");
            connect(anim, &QPropertyAnimation::valueChanged, this, [=]() { update(); });
            connect(anim, &QPropertyAnimation::finished, this, [=]() {
                _pIsIndicatorAnimationFinished = true;
                update();
            });
            anim->setDuration(300);
            anim->setEasingCurve(QEasingCurve::InOutSine);
            anim->setStartValue((_pIndicatorY != 0) ? _pIndicatorY : prevY);
            anim->setEndValue(targetY);
            anim->start(QAbstractAnimation::DeleteWhenStopped);
        }
        else
        {
            // First selection: grow from center
            _pIndicatorAnimationHeight = 0;
            QPropertyAnimation* anim = new QPropertyAnimation(this, "pIndicatorY");
            connect(anim, &QPropertyAnimation::valueChanged, this, [=]() { update(); });
            connect(anim, &QPropertyAnimation::finished, this, [=]() {
                _pIsIndicatorAnimationFinished = true;
                update();
            });
            anim->setDuration(300);
            anim->setEasingCurve(QEasingCurve::InOutSine);
            anim->setStartValue(currentRect.center().y());
            anim->setEndValue(targetY);
            anim->start(QAbstractAnimation::DeleteWhenStopped);

            QPropertyAnimation* heightAnim = new QPropertyAnimation(this, "pIndicatorAnimationHeight");
            heightAnim->setDuration(300);
            heightAnim->setEasingCurve(QEasingCurve::InOutSine);
            heightAnim->setStartValue(0);
            heightAnim->setEndValue(targetHeight);
            heightAnim->start(QAbstractAnimation::DeleteWhenStopped);
        }

        _pIsIndicatorAnimationFinished = false;
        _pIndicatorHeight = targetHeight;
        return;
    }

    // ── Horizontal animation (Top/Bottom) / static (Left) ──
    // Only animate horizontal indicators (Top/Bottom); Left stays static
    if (getIndicatorPosition() == ElaTabBarType::Left || !_pIsIndicatorAnimationFinished)
    {
        _pIsIndicatorAnimationFinished = true;
        _previousTabRect = (currentIndex >= 0) ? tabRect(currentIndex) : QRect();
        update();
        return;
    }

    _previousTabRect = (previousIndex >= 0) ? tabRect(previousIndex) : QRect();
    QRect currentRect = tabRect(currentIndex);
    if (!currentRect.isValid())
    {
        _pIsIndicatorAnimationFinished = true;
        update();
        return;
    }

    const int horizontalPadding = 16; // margin(9) + 7
    int targetX = currentRect.left() + horizontalPadding;
    int targetWidth = currentRect.width() - 2 * horizontalPadding;

    if (previousIndex >= 0 && _previousTabRect.isValid())
    {
        // Slide between existing tabs
        int prevX = _previousTabRect.left() + horizontalPadding;
        QPropertyAnimation* anim = new QPropertyAnimation(this, "pIndicatorX");
        connect(anim, &QPropertyAnimation::valueChanged, this, [=]() { update(); });
        connect(anim, &QPropertyAnimation::finished, this, [=]() {
            _pIsIndicatorAnimationFinished = true;
            update();
        });
        anim->setDuration(300);
        anim->setEasingCurve(QEasingCurve::InOutSine);
        anim->setStartValue((_pIndicatorX != 0) ? _pIndicatorX : prevX);
        anim->setEndValue(targetX);
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    }
    else
    {
        // First selection: grow indicator from center
        _pIndicatorAnimationWidth = 0;
        QPropertyAnimation* anim = new QPropertyAnimation(this, "pIndicatorX");
        connect(anim, &QPropertyAnimation::valueChanged, this, [=]() { update(); });
        connect(anim, &QPropertyAnimation::finished, this, [=]() {
            _pIsIndicatorAnimationFinished = true;
            update();
        });
        anim->setDuration(300);
        anim->setEasingCurve(QEasingCurve::InOutSine);
        anim->setStartValue(currentRect.center().x());
        anim->setEndValue(targetX);
        anim->start(QAbstractAnimation::DeleteWhenStopped);

        QPropertyAnimation* widthAnim = new QPropertyAnimation(this, "pIndicatorAnimationWidth");
        widthAnim->setDuration(300);
        widthAnim->setEasingCurve(QEasingCurve::InOutSine);
        widthAnim->setStartValue(0);
        widthAnim->setEndValue(targetWidth);
        widthAnim->start(QAbstractAnimation::DeleteWhenStopped);
    }

    _pIsIndicatorAnimationFinished = false;
    _pIndicatorWidth = targetWidth;
}