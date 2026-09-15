/**
 * @file   TerminalTabWidget.cpp
 * @brief  终端页面专用标签控件实现。
 */
#include "TerminalTabWidget.h"

#include "ElaIconButton.h"
#include <ElaMenu.h>
#include <ElaTabBar.h>
#include "service/LanguageManager.h"

#include <QAction>
#include <QCursor>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPointer>

TerminalTabWidget::TerminalTabWidget(QWidget* parent)
    : ElaTabWidget(parent)
{
    auto* const tabs = elaTabBar();
    const int compactWidth = QFontMetrics(tabs->font()).horizontalAdvance(
        QStringLiteral("192.168.10.100")) + TabHorizontalChrome;
    setTabSize(QSize(compactWidth, getTabSize().height()));
    tabs->setExpanding(false);
    tabs->setMouseTracking(true);
    tabs->installEventFilter(this);
    tabs->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tabs, &QWidget::customContextMenuRequested,
            this, &TerminalTabWidget::showTabContextMenu);

    _hoverSwitchTimer.setSingleShot(true);
    _hoverSwitchTimer.setInterval(HoverSwitchDelayMs);
    connect(&_hoverSwitchTimer, &QTimer::timeout, this, [this]() {
        const int pendingIndex = _pendingHoverIndex;
        _pendingHoverIndex = -1;
        if (pendingIndex < 0 || pendingIndex >= count())
            return;

        auto* const tabs = elaTabBar();
        const QPoint position = tabs->mapFromGlobal(QCursor::pos());
        if (tabTitleAt(position) == pendingIndex
            && currentIndex() != pendingIndex) {
            setCurrentIndex(pendingIndex);
        }
    });
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateConnectionActions(); });
}

TerminalTabWidget::~TerminalTabWidget()
{
    // 派生成员先于 ElaTabWidget 基类析构；基类随后删除标签页时，不得再让
    // destroyed 回调访问已经析构的动作映射。
    for (QWidget* page : _connectionButtons.keys())
        QObject::disconnect(page, nullptr, this, nullptr);
}

ElaTabBar* TerminalTabWidget::elaTabBar() const noexcept
{
    // ElaTabWidget 构造函数无条件安装 ElaTabBar，静态转换保留准确类型。
    return static_cast<ElaTabBar*>(tabBar());
}

int TerminalTabWidget::tabTitleAt(const QPoint& position) const
{
    auto* const tabs = elaTabBar();
    const int index = tabs->tabAt(position);
    if (index < 0 || index >= count() || !tabs->isTabEnabled(index))
        return -1;

    for (const auto side : {QTabBar::LeftSide, QTabBar::RightSide}) {
        QWidget* const button = tabs->tabButton(index, side);
        if (!button || !button->isVisible())
            continue;
        const QRect buttonRect(button->mapTo(tabs, QPoint{}), button->size());
        if (buttonRect.contains(position))
            return -1;
    }
    return index;
}

void TerminalTabWidget::scheduleHoverSwitch(const QPoint& position)
{
    const int index = tabTitleAt(position);
    if (index < 0 || index == currentIndex()) {
        cancelHoverSwitch();
        return;
    }
    if (_pendingHoverIndex == index && _hoverSwitchTimer.isActive())
        return;

    _pendingHoverIndex = index;
    _hoverSwitchTimer.start();
}

void TerminalTabWidget::cancelHoverSwitch()
{
    _hoverSwitchTimer.stop();
    _pendingHoverIndex = -1;
}

void TerminalTabWidget::tabInserted(int index)
{
    ElaTabWidget::tabInserted(index);
    setTabToolTip(index, tabText(index));
    installConnectionAction(index);
}

void TerminalTabWidget::setTabText(int index, const QString& text)
{
    ElaTabWidget::setTabText(index, text);
    setTabToolTip(index, text);
}

void TerminalTabWidget::installConnectionAction(int index)
{
    QWidget* const page = widget(index);
    if (!page || _connectionButtons.contains(page))
        return;

    auto* const tabs = elaTabBar();
    auto* const buttonArea = new QWidget(tabs);
    buttonArea->setFixedSize(ConnectionButtonExtent * 2 + 2,
                             ConnectionButtonExtent);
    auto* const layout = new QHBoxLayout(buttonArea);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto* const actionButton = new ElaIconButton(
        ElaIconType::PowerOff, 11, ConnectionButtonExtent,
        ConnectionButtonExtent, buttonArea);
    actionButton->setProperty("novatermConnectionActionButton", true);
    actionButton->hide();
    layout->addWidget(actionButton);
    layout->addStretch();

    auto* const closeButton = new ElaIconButton(
        ElaIconType::Xmark, 11, ConnectionButtonExtent,
        ConnectionButtonExtent, buttonArea);
    closeButton->setProperty("novatermCloseButton", true);
    closeButton->setToolTip(tr("Close terminal"));
    closeButton->setAccessibleName(tr("Close terminal"));
    layout->addWidget(closeButton);

    tabs->setTabButton(index, QTabBar::RightSide, buttonArea);

    const QPointer<QWidget> pageGuard(page);
    connect(closeButton, &ElaIconButton::clicked, this, [this, pageGuard]() {
        if (!pageGuard)
            return;
        const int currentIndex = indexOf(pageGuard);
        if (currentIndex >= 0)
            emit elaTabBar()->tabCloseRequested(currentIndex);
    });
    connect(actionButton, &ElaIconButton::clicked,
            this, [this, pageGuard]() {
        if (!pageGuard)
            return;
        const auto action = _connectionActions.value(
            pageGuard, ConnectionAction::Hidden);
        if (action == ConnectionAction::Disconnect)
            emit disconnectRequested(pageGuard);
        else if (action == ConnectionAction::Reconnect)
            emit reconnectRequested(pageGuard);
    });
    connect(page, &QObject::destroyed, this, [this, page]() {
        _connectionButtons.remove(page);
        _connectionActions.remove(page);
    });

    _connectionButtons.insert(page, actionButton);
    _connectionActions.insert(page, ConnectionAction::Hidden);
}

void TerminalTabWidget::setTabConnectionAction(
    QWidget* page, ConnectionAction action)
{
    if (!page)
        return;
    const auto button = _connectionButtons.value(page);
    if (!button)
        return;
    _connectionActions.insert(page, action);
    applyConnectionAction(button, action);
}

void TerminalTabWidget::applyConnectionAction(
    ElaIconButton* button, ConnectionAction action)
{
    if (!button)
        return;
    if (action == ConnectionAction::Hidden) {
        button->hide();
        return;
    }

    const bool disconnect = action == ConnectionAction::Disconnect;
    button->setAwesome(disconnect
        ? ElaIconType::PowerOff : ElaIconType::ArrowRotateRight);
    const QString text = disconnect ? tr("Disconnect") : tr("Reconnect");
    button->setToolTip(text);
    button->setAccessibleName(text);
    button->show();
}

void TerminalTabWidget::retranslateConnectionActions()
{
    for (auto it = _connectionButtons.cbegin();
         it != _connectionButtons.cend(); ++it) {
        if (it.value())
            applyConnectionAction(it.value(), _connectionActions.value(it.key()));
    }
    for (int index = 0; index < count(); ++index) {
        QWidget* const area = elaTabBar()->tabButton(
            index, QTabBar::RightSide);
        if (!area)
            continue;
        const auto buttons = area->findChildren<ElaIconButton*>();
        for (auto* button : buttons) {
            if (!button->property("novatermCloseButton").toBool())
                continue;
            button->setToolTip(tr("Close terminal"));
            button->setAccessibleName(tr("Close terminal"));
        }
    }
}

bool TerminalTabWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != elaTabBar())
        return ElaTabWidget::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::Enter:
        scheduleHoverSwitch(
            elaTabBar()->mapFromGlobal(QCursor::pos()));
        break;
    case QEvent::MouseMove: {
        const auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->buttons() == Qt::NoButton)
            scheduleHoverSwitch(mouseEvent->position().toPoint());
        else
            cancelHoverSwitch();
        break;
    }
    case QEvent::Leave:
    case QEvent::Hide:
    case QEvent::MouseButtonPress:
        cancelHoverSwitch();
        break;
    default:
        break;
    }
    return ElaTabWidget::eventFilter(watched, event);
}

void TerminalTabWidget::showTabContextMenu(const QPoint& position)
{
    auto* const tabs = elaTabBar();
    const int index = tabs->tabAt(position);
    if (index < 0 || index >= count())
        return;

    // 与历史会话面板保持一致：右键操作的标签同时成为当前标签，避免菜单目标
    // 与页面当前内容不一致。
    setCurrentIndex(index);

    auto* menu = new ElaMenu(tabs);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setMenuItemHeight(27);
    QAction* const editAction = menu->addElaIconAction(
        ElaIconType::PenToSquare, tr("Edit"));
    const QPointer<QWidget> target = widget(index);
    connect(editAction, &QAction::triggered, this,
            [this, target]() {
        if (!target)
            return;
        const int currentIndex = indexOf(target);
        if (currentIndex >= 0)
            emit editSessionRequested(currentIndex);
    });
    menu->popup(tabs->mapToGlobal(position));
}
