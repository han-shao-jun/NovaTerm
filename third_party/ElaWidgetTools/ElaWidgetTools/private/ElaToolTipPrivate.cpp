#include "ElaToolTipPrivate.h"

#include <QEvent>
#include <QPropertyAnimation>
#include <QTimer>

#include "ElaToolTip.h"
ElaToolTipPrivate::ElaToolTipPrivate(QObject* parent)
    : QObject{parent}
{
    _pOpacity = 1;

    // Create timers used to coordinate show/hide/display to avoid races
    _showTimer = new QTimer(this);
    _showTimer->setSingleShot(true);
    connect(_showTimer, &QTimer::timeout, this, [=]() {
        _doShowAnimation();
    });

    _hideTimer = new QTimer(this);
    _hideTimer->setSingleShot(true);
    connect(_hideTimer, &QTimer::timeout, this, [=]() {
        Q_Q(ElaToolTip);
        if (q->isVisible())
            q->hide();
        if (_displayTimer && _displayTimer->isActive())
            _displayTimer->stop();
    });

    _displayTimer = new QTimer(this);
    _displayTimer->setSingleShot(true);
    connect(_displayTimer, &QTimer::timeout, this, [=]() {
        Q_Q(ElaToolTip);
        if (q->isVisible())
            q->hide();
    });
}

ElaToolTipPrivate::~ElaToolTipPrivate()
{
}

bool ElaToolTipPrivate::eventFilter(QObject* watched, QEvent* event)
{
    Q_Q(ElaToolTip);
    switch (event->type())
    {
    case QEvent::Enter:
    {
        // Cancel any pending hide, and start (or restart) the show timer.
        if (_hideTimer && _hideTimer->isActive())
            _hideTimer->stop();
        if (_showTimer)
            _showTimer->start(qMax(0, _pShowDelayMsec));
        break;
    }
    case QEvent::Leave:
    {
        // If tooltip hasn't been shown yet, cancel the pending show.
        if (_showTimer && _showTimer->isActive())
            _showTimer->stop();
        // Start hide timer to defer hiding (honor HideDelayMsec).
        if (_hideTimer)
            _hideTimer->start(qMax(0, _pHideDelayMsec));
        // Also stop display timer if running.
        if (_displayTimer && _displayTimer->isActive())
            _displayTimer->stop();
        break;
    }
    case QEvent::HoverMove:
    case QEvent::MouseMove:
    {
        _updatePos();
        break;
    }
    default:
    {
        break;
    }
    }
    return QObject::eventFilter(watched, event);
}

void ElaToolTipPrivate::_doShowAnimation()
{
    Q_Q(ElaToolTip);
    // If already visible, just update position and restart display timer.
    if (q->isVisible())
    {
        _updatePos();
        if (_pDisplayMsec > -1 && _displayTimer)
            _displayTimer->start(_pDisplayMsec);
        return;
    }

    // Cancel any pending hide to avoid immediate hide after show.
    if (_hideTimer && _hideTimer->isActive())
        _hideTimer->stop();

    QPoint cursorPoint = QCursor::pos();
    q->move(cursorPoint.x() + 10, cursorPoint.y());
    q->show();

    // Fade-in animation
    QPropertyAnimation* showAnimation = new QPropertyAnimation(q, "windowOpacity");
    showAnimation->setEasingCurve(QEasingCurve::InOutSine);
    showAnimation->setDuration(250);
    showAnimation->setStartValue(0);
    showAnimation->setEndValue(1);
    showAnimation->start(QAbstractAnimation::DeleteWhenStopped);

    // Start display timer if configured (hide after shown for a duration).
    if (_pDisplayMsec > -1 && _displayTimer)
    {
        _displayTimer->start(_pDisplayMsec);
    }
}

void ElaToolTipPrivate::_updatePos()
{
    Q_Q(ElaToolTip);
    if (q->isVisible())
    {
        QPoint cursorPoint = QCursor::pos();
        q->move(cursorPoint.x() + 10, cursorPoint.y());
    }
}
