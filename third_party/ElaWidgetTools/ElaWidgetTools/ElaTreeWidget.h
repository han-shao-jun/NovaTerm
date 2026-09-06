#ifndef ELATREEWIDGET_H
#define ELATREEWIDGET_H

#include <QTreeWidget>

#include "ElaProperty.h"

// ElaTreeView 的 item 版本。ElaWidgetTools 原本只提供基于 QTreeView 的
// ElaTreeView，需要 QTreeWidget/QTreeWidgetItem 便捷 API 的调用方无法在保留
// item 模型的同时获得 Fluent 外观。本类以 ElaTreeView 的构造流程为准，换用
// QTreeWidget 作为基类，因此两者视觉完全一致。
//
// 之所以必须放在库内：绘制所依赖的 ElaTreeViewStyle 位于 DeveloperComponents
// 且没有 ELA_EXPORT，库外无法实例化。
class ElaTreeWidgetPrivate;
class ELA_EXPORT ElaTreeWidget : public QTreeWidget
{
    Q_OBJECT
    Q_Q_CREATE(ElaTreeWidget)
    Q_PROPERTY_CREATE_Q_H(int, ItemHeight)
    Q_PROPERTY_CREATE_Q_H(int, HeaderMargin)
public:
    explicit ElaTreeWidget(QWidget* parent = nullptr);
    ~ElaTreeWidget();

    /**
     * @brief 设置首列内容的附加左边距（逻辑像素，默认 11）。
     * @note 设为 0 时，无图标和复选框的首列文字与单元格左边缘对齐。
     */
    void setItemLeftPadding(int padding);
    /** @brief 返回首列内容的附加左边距。 */
    [[nodiscard]] int getItemLeftPadding() const;

    /**
     * 关闭视口外框（圆角边线 + 底色填充）。
     *
     * 默认 true，与 ElaTreeView 表现一致。置 false 后本控件完全透明、无边线，
     * 用于已经嵌在卡片或分组框里的场合 —— 此时外层容器已提供边界，再画一圈会
     * 显得重复。注意 setFrameShape(QFrame::NoFrame) 达不到这个效果：它只让
     * frameWidth 归零，Qt 仍会向 style 派发 CE_ShapedFrame。
     */
    void setIsFrameVisible(bool visible);
    [[nodiscard]] bool getIsFrameVisible() const;
};

#endif // ELATREEWIDGET_H
