#include "ElaComboBox.h"

#include "ElaComboBoxStyle.h"
#include "ElaScrollBar.h"
#include "ElaTheme.h"
#include "private/ElaComboBoxPrivate.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QLineEdit>
#include <QListView>
#include <QPropertyAnimation>
Q_PROPERTY_CREATE_Q_CPP(ElaComboBox, int, BorderRadius)
ElaComboBox::ElaComboBox(QWidget* parent)
    : QComboBox(parent), d_ptr(new ElaComboBoxPrivate())
{
    Q_D(ElaComboBox);
    d->q_ptr = this;
    d->_pBorderRadius = 3;
    d->_themeMode = eTheme->getThemeMode();
    setObjectName("ElaComboBox");
    setFixedHeight(35);
    d->_comboBoxStyle = new ElaComboBoxStyle(style());
    setStyle(d->_comboBoxStyle);

    //调用view 让container初始化
    setView(new QListView(this));
    QAbstractItemView* comboBoxView = this->view();
    comboBoxView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    ElaScrollBar* scrollBar = new ElaScrollBar(this);
    comboBoxView->setVerticalScrollBar(scrollBar);
    ElaScrollBar* floatVScrollBar = new ElaScrollBar(scrollBar, comboBoxView);
    floatVScrollBar->setIsAnimation(true);
    comboBoxView->setAutoScroll(false);
    comboBoxView->setSelectionMode(QAbstractItemView::NoSelection);
    comboBoxView->setObjectName("ElaComboBoxView");
    comboBoxView->setStyleSheet("#ElaComboBoxView{background-color:transparent;}");
    comboBoxView->setStyle(d->_comboBoxStyle);
    QWidget* container = this->findChild<QFrame*>();
    if (container)
    {
        container->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        container->setAttribute(Qt::WA_TranslucentBackground);
        container->setObjectName("ElaComboBoxContainer");
        container->setStyle(d->_comboBoxStyle);
#ifndef Q_OS_WIN
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        container->setStyleSheet("background-color:transparent;");
#endif
#endif
    }
    QComboBox::setMaxVisibleItems(5);
    connect(eTheme, &ElaTheme::themeModeChanged, d, &ElaComboBoxPrivate::onThemeChanged);
}

ElaComboBox::~ElaComboBox()
{
    Q_D(ElaComboBox);
    delete d->_comboBoxStyle;
}

void ElaComboBox::setEditable(bool editable)
{
    Q_D(ElaComboBox);
    QComboBox::setEditable(editable);
    if (editable)
    {
        lineEdit()->setStyle(d->_comboBoxStyle);
        d->onThemeChanged(d->_themeMode);
    }
}

void ElaComboBox::showPopup()
{
    Q_D(ElaComboBox);
    bool oldAnimationEffects = qApp->isEffectEnabled(Qt::UI_AnimateCombo);
    qApp->setEffectEnabled(Qt::UI_AnimateCombo, false);
    QComboBox::showPopup();
    qApp->setEffectEnabled(Qt::UI_AnimateCombo, oldAnimationEffects);
    if (count() > 0)
    {
        // QComboBox owns the popup container layout. Reparenting its view or
        // removing the private layout items leaves Qt with a null layout item
        // and crashes in QLayout::activate() on subsequent popup operations.
        d->_isAllowHidePopup = true;

        //指示器动画
        QPropertyAnimation* rotateAnimation = new QPropertyAnimation(d->_comboBoxStyle, "pExpandIconRotate");
        connect(rotateAnimation, &QPropertyAnimation::valueChanged, this, [=](const QVariant& value) {
            update();
        });
        rotateAnimation->setDuration(300);
        rotateAnimation->setEasingCurve(QEasingCurve::InOutSine);
        rotateAnimation->setStartValue(d->_comboBoxStyle->getExpandIconRotate());
        rotateAnimation->setEndValue(-180);
        rotateAnimation->start(QAbstractAnimation::DeleteWhenStopped);
        QPropertyAnimation* markAnimation = new QPropertyAnimation(d->_comboBoxStyle, "pExpandMarkWidth");
        markAnimation->setDuration(300);
        markAnimation->setEasingCurve(QEasingCurve::InOutSine);
        markAnimation->setStartValue(d->_comboBoxStyle->getExpandMarkWidth());
        markAnimation->setEndValue(width() / 2 - d->_pBorderRadius - 6);
        markAnimation->start(QAbstractAnimation::DeleteWhenStopped);
    }
}

void ElaComboBox::hidePopup()
{
    Q_D(ElaComboBox);
    if (d->_isAllowHidePopup)
    {
        QComboBox::hidePopup();
        d->_isAllowHidePopup = false;

        //指示器动画
        QPropertyAnimation* rotateAnimation = new QPropertyAnimation(d->_comboBoxStyle, "pExpandIconRotate");
        connect(rotateAnimation, &QPropertyAnimation::valueChanged, this, [=](const QVariant& value) {
            update();
        });
        rotateAnimation->setDuration(300);
        rotateAnimation->setEasingCurve(QEasingCurve::InOutSine);
        rotateAnimation->setStartValue(d->_comboBoxStyle->getExpandIconRotate());
        rotateAnimation->setEndValue(0);
        rotateAnimation->start(QAbstractAnimation::DeleteWhenStopped);
        QPropertyAnimation* markAnimation = new QPropertyAnimation(d->_comboBoxStyle, "pExpandMarkWidth");
        markAnimation->setDuration(300);
        markAnimation->setEasingCurve(QEasingCurve::InOutSine);
        markAnimation->setStartValue(d->_comboBoxStyle->getExpandMarkWidth());
        markAnimation->setEndValue(0);
        markAnimation->start(QAbstractAnimation::DeleteWhenStopped);
    }
}

void ElaComboBox::paintEvent(QPaintEvent* event)
{
    Q_D(ElaComboBox);
    if (lineEdit() && lineEdit()->palette().color(QPalette::Text) != ElaThemeColor(d->_themeMode, BasicText))
    {
        d->onThemeChanged(d->_themeMode);
    }
    QComboBox::paintEvent(event);
}
