#include "ElaTreeWidgetStyle.h"

ElaTreeWidgetStyle::ElaTreeWidgetStyle(QStyle* style)
    : ElaTreeViewStyle(style)
{
    _pIsFrameVisible = true;
}

ElaTreeWidgetStyle::~ElaTreeWidgetStyle()
{
}

void ElaTreeWidgetStyle::drawControl(ControlElement element, const QStyleOption* option, QPainter* painter, const QWidget* widget) const
{
    // 外框关闭时直接吞掉该元素，控件即完全透明、无边线；其余元素一律交回基类，
    // 保证与 ElaTreeView 的绘制逐像素一致。
    if (element == QStyle::CE_ShapedFrame && !_pIsFrameVisible)
    {
        return;
    }
    ElaTreeViewStyle::drawControl(element, option, painter, widget);
}
