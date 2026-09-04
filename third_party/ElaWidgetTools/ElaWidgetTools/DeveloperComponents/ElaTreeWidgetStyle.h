#ifndef ELATREEWIDGETSTYLE_H
#define ELATREEWIDGETSTYLE_H

#include "ElaTreeViewStyle.h"

// ElaTreeWidget 的绘制样式。除「视口外框可关」之外与 ElaTreeViewStyle 完全一致，
// 因此直接继承而非复制一份实现。
//
// 之所以需要这个开关：Qt 对 item view 无条件向 style 派发 CE_ShapedFrame，
// setFrameShape(QFrame::NoFrame) 只让 frameWidth 归零，挡不住基类在该元素里画
// 圆角边线与 BasicBaseAlpha 底色。控件已嵌在卡片内时那圈边框是多余的。
class ElaTreeWidgetStyle : public ElaTreeViewStyle
{
    Q_OBJECT
    Q_PROPERTY_CREATE(bool, IsFrameVisible)
public:
    explicit ElaTreeWidgetStyle(QStyle* style = nullptr);
    ~ElaTreeWidgetStyle() override;
    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter, const QWidget* widget = nullptr) const override;
};

#endif // ELATREEWIDGETSTYLE_H
