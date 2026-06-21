#ifndef ELATABBAR_H
#define ELATABBAR_H

#include <QDrag>
#include <QTabBar>

#include "ElaDef.h"
#include "ElaProperty.h"
class ElaTabBarPrivate;
class ELA_EXPORT ElaTabBar : public QTabBar
{
    Q_OBJECT
    Q_Q_CREATE(ElaTabBar)
    Q_PROPERTY_CREATE_Q_H(QSize, TabSize)
    Q_PROPERTY_CREATE_Q_H(ElaTabBarType::IndicatorPosition, IndicatorPosition)

    // Animation properties for horizontal indicator slide
    Q_PROPERTY_CREATE(int, IndicatorX)
    Q_PRIVATE_CREATE(int, IndicatorWidth)
    Q_PROPERTY_CREATE(int, IndicatorAnimationWidth)
    Q_PROPERTY_CREATE(bool, IsIndicatorAnimationFinished)
    int _previousIndex{-1};
    QRect _previousTabRect;

public:
    void doIndicatorAnimation(int previousIndex, int currentIndex);
    explicit ElaTabBar(QWidget* parent = nullptr);
    ~ElaTabBar() override;
Q_SIGNALS:
    Q_SIGNAL void tabDragCreate(QMimeData* mimeData);
    Q_SIGNAL void tabDragEnter(QMimeData* mimeData);
    Q_SIGNAL void tabDragLeave(QMimeData* mimeData);
    Q_SIGNAL void tabDragDrop(QMimeData* mimeData);

protected:
    QSize sizeHint() const;
    virtual void mouseMoveEvent(QMouseEvent* event) override;
    virtual void dragEnterEvent(QDragEnterEvent* event) override;
    virtual void dragMoveEvent(QDragMoveEvent* event) override;
    virtual void dragLeaveEvent(QDragLeaveEvent* event) override;
    virtual void dropEvent(QDropEvent* event) override;
    virtual void wheelEvent(QWheelEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
};

#endif // ELATABBAR_H
