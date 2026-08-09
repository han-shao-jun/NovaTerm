#pragma once

#include <ElaTabWidget.h>

class QEvent;
class QResizeEvent;

// ElaTabWidget extension that places one action widget immediately after the
// last tab while reserving space for it when the tab bar overflows.
class TabActionWidget final : public ElaTabWidget
{
public:
    explicit TabActionWidget(QWidget* parent = nullptr);

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
