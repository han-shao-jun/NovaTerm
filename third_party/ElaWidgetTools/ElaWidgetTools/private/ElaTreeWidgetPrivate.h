#ifndef ELATREEWIDGETPRIVATE_H
#define ELATREEWIDGETPRIVATE_H

#include <QObject>

#include "ElaProperty.h"

class ElaTreeWidget;
class ElaTreeWidgetStyle;
class ElaTreeWidgetPrivate : public QObject
{
    Q_OBJECT
    Q_D_CREATE(ElaTreeWidget)
public:
    explicit ElaTreeWidgetPrivate(QObject* parent = nullptr);
    ~ElaTreeWidgetPrivate();

private:
    // 派生自 ElaTreeViewStyle，绘制与 ElaTreeView 完全一致，仅多一个外框开关。
    ElaTreeWidgetStyle* _treeWidgetStyle{nullptr};
};

#endif // ELATREEWIDGETPRIVATE_H
