/**
 * @file   TabActionWidget.h
 * @brief  ElaTabWidget 扩展：标签栏尾部动作控件。
 *
 * 在最后一个标签之后放置一个动作控件，并在标签栏溢出时为其预留空间。
 */
#pragma once

#include <ElaTabWidget.h>

class QEvent;
class QResizeEvent;

// ElaTabWidget 扩展：在最后一个标签之后放置一个动作控件，
// 标签栏溢出时仍为其预留空间。
class TabActionWidget final : public ElaTabWidget
{
public:
    explicit TabActionWidget(QWidget* parent = nullptr);

    /**
     * @brief 设置标签栏尾部动作控件。
     * @param widget 动作控件（本对象不接管其所有权）。
     */
    void setTabBarActionWidget(QWidget* widget);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void tabInserted(int index) override;
    void tabRemoved(int index) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void scheduleActionWidgetLayout();
    void updateActionWidgetGeometry();

    QWidget* _actionWidget{nullptr};
    bool _layoutPending{false};
};
