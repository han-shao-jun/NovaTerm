#include "ElaTreeWidget.h"

#include "ElaScrollBar.h"
#include "ElaTreeWidgetPrivate.h"
#include "ElaTreeWidgetStyle.h"
ElaTreeWidget::ElaTreeWidget(QWidget* parent)
    : QTreeWidget(parent), d_ptr(new ElaTreeWidgetPrivate())
{
    Q_D(ElaTreeWidget);
    d->q_ptr = this;
    setObjectName("ElaTreeWidget");
    setStyleSheet(
        "#ElaTreeWidget{background-color:transparent;}"
        "QHeaderView{background-color:transparent;border:0px;}");

    setAnimated(true);
    setMouseTracking(true);

    ElaScrollBar* hScrollBar = new ElaScrollBar(this);
    hScrollBar->setIsAnimation(true);
    connect(hScrollBar, &ElaScrollBar::rangeAnimationFinished, this, [=]() {
        doItemsLayout();
    });
    setHorizontalScrollBar(hScrollBar);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    ElaScrollBar* vScrollBar = new ElaScrollBar(this);
    vScrollBar->setIsAnimation(true);
    connect(vScrollBar, &ElaScrollBar::rangeAnimationFinished, this, [=]() {
        doItemsLayout();
    });
    setVerticalScrollBar(vScrollBar);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    // 必须最后设置：setStyleSheet() 已让 Qt 为本控件装上 QStyleSheetStyle，
    // 后者在创建时即缓存 baseStyle。顺序颠倒会让 ElaTreeWidgetStyle 无法参与绘制。
    d->_treeWidgetStyle = new ElaTreeWidgetStyle(style());
    setStyle(d->_treeWidgetStyle);
}

ElaTreeWidget::~ElaTreeWidget()
{
    Q_D(ElaTreeWidget);
    delete d->_treeWidgetStyle;
}

void ElaTreeWidget::setItemHeight(int itemHeight)
{
    Q_D(ElaTreeWidget);
    if (itemHeight > 0)
    {
        d->_treeWidgetStyle->setItemHeight(itemHeight);
        doItemsLayout();
    }
}

void ElaTreeWidget::setItemLeftPadding(int padding)
{
    Q_D(ElaTreeWidget);
    if (padding >= 0)
    {
        d->_treeWidgetStyle->setItemLeftPadding(padding);
        doItemsLayout();
        viewport()->update();
    }
}

int ElaTreeWidget::getItemLeftPadding() const
{
    Q_D(const ElaTreeWidget);
    return d->_treeWidgetStyle->getItemLeftPadding();
}

int ElaTreeWidget::getItemHeight() const
{
    Q_D(const ElaTreeWidget);
    return d->_treeWidgetStyle->getItemHeight();
}

void ElaTreeWidget::setHeaderMargin(int headerMargin)
{
    Q_D(ElaTreeWidget);
    if (headerMargin >= 0)
    {
        d->_treeWidgetStyle->setHeaderMargin(headerMargin);
        doItemsLayout();
    }
}

int ElaTreeWidget::getHeaderMargin() const
{
    Q_D(const ElaTreeWidget);
    return d->_treeWidgetStyle->getHeaderMargin();
}

void ElaTreeWidget::setIsFrameVisible(bool visible)
{
    Q_D(ElaTreeWidget);
    d->_treeWidgetStyle->setIsFrameVisible(visible);
    update();
}

bool ElaTreeWidget::getIsFrameVisible() const
{
    Q_D(const ElaTreeWidget);
    return d->_treeWidgetStyle->getIsFrameVisible();
}
