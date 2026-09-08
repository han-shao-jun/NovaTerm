#ifndef ELACUSTOMTABWIDGET_H
#define ELACUSTOMTABWIDGET_H

#include "ElaCustomWidget.h"

class ElaTabBar;
class ElaTabWidget;
class ElaCustomTabWidget : public ElaCustomWidget
{
    Q_OBJECT
    Q_PROPERTY_CREATE(bool, IsFinished)
public:
    explicit ElaCustomTabWidget(QWidget* parent = nullptr);
    ~ElaCustomTabWidget() override;
    void addTab(QWidget* widget, QIcon tabIcon, const QString& tabTitle);
    ElaTabBar* getCustomTabBar() const;
    ElaTabWidget* getCustomTabWidget() const;
    /**
     * @brief 拖拽期间启用输入穿透；结束时可靠恢复窗口交互。
     * @param transparent 是否启用窗口输入穿透。
     */
    void setDragInputTransparent(bool transparent);

    Q_INVOKABLE bool processHitTest();

private:
    bool _isAllowLeave{false};
    ElaTabBar* _customTabBar{nullptr};
    ElaTabWidget* _customTabWidget{nullptr};
};

#endif // ELACUSTOMTABWIDGET_H
