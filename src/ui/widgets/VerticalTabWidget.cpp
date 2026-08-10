/**
 * @file   VerticalTabWidget.cpp
 * @brief  垂直选项卡复合控件实现。
 *
 * 左侧 ElaPushButton 按钮组 + 右侧 ElaCentralStackedWidget 堆栈页面。
 * 按钮点击切换堆栈页面，API 对齐 QTabWidget。
 */
#include "VerticalTabWidget.h"

#include "ElaCentralStackedWidget.h"
#include "ElaPushButton.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>

VerticalTabWidget::VerticalTabWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ── 左侧选项卡栏 ──────────────────────────────
    auto* leftWidget = new QWidget(this);
    leftWidget->setFixedWidth(130);
    _buttonLayout = new QVBoxLayout(leftWidget);
    _buttonLayout->setContentsMargins(8, 8, 8, 8);
    _buttonLayout->setSpacing(4);
    _buttonLayout->addStretch();   // 底部弹簧，按钮从上方堆叠
    mainLayout->addWidget(leftWidget);

    // ── 右侧堆栈页面 ──────────────────────────────
    _stackedWidget = new ElaCentralStackedWidget(this);
    mainLayout->addWidget(_stackedWidget, 1);

    // ── 按钮互斥组 ────────────────────────────────
    _buttonGroup = new QButtonGroup(this);
    _buttonGroup->setExclusive(true);
    connect(_buttonGroup, &QButtonGroup::idClicked,
            this, &VerticalTabWidget::setCurrentIndex);
}

int VerticalTabWidget::addTab(QWidget* page, const QString& label)
{
    const int id = _buttons.size();

    auto* button = new ElaPushButton(label, this);
    button->setCheckable(true);

    _buttonGroup->addButton(button, id);
    // 插入到 stretch 之前（stretch 始终是最后一项）
    _buttonLayout->insertWidget(_buttonLayout->count() - 1, button);
    _stackedWidget->getContainerStackedWidget()->addWidget(page);

    _buttons.append(button);

    // 第一个标签页自动选中
    if (_buttons.size() == 1)
        setCurrentIndex(0);

    return id;
}

int VerticalTabWidget::count() const
{
    return _buttons.size();
}

int VerticalTabWidget::currentIndex() const
{
    return _currentIndex;
}

QWidget* VerticalTabWidget::widget(int index) const
{
    if (index < 0 || index >= _buttons.size())
        return nullptr;
    return _stackedWidget->getContainerStackedWidget()->widget(index);
}

QWidget* VerticalTabWidget::currentWidget() const
{
    return widget(_currentIndex);
}

QString VerticalTabWidget::tabText(int index) const
{
    if (index < 0 || index >= _buttons.size())
        return {};
    return _buttons.at(index)->text();
}

void VerticalTabWidget::setTabText(int index, const QString& text)
{
    if (index < 0 || index >= _buttons.size())
        return;
    _buttons.at(index)->setText(text);
}

void VerticalTabWidget::setCurrentIndex(int index)
{
    if (index < 0 || index >= _buttons.size() || index == _currentIndex)
        return;

    _currentIndex = index;
    _buttonGroup->button(index)->setChecked(true);
    _stackedWidget->getContainerStackedWidget()->setCurrentIndex(index);
    updateButtonSelection();
    emit currentChanged(index);
}

void VerticalTabWidget::removeTab(int index)
{
    if (index < 0 || index >= _buttons.size())
        return;

    // 移除按钮
    auto* button = _buttons.takeAt(index);
    _buttonGroup->removeButton(button);
    _buttonLayout->removeWidget(button);
    delete button;

    // 移除页面（不 delete，与 QTabWidget 行为一致：调用者拥有页面所有权）
    _stackedWidget->getContainerStackedWidget()->removeWidget(
        _stackedWidget->getContainerStackedWidget()->widget(index));

    // 重新映射 QButtonGroup ID
    for (int i = 0; i < _buttons.size(); ++i)
        _buttonGroup->setId(_buttons[i], i);

    // 调整当前索引
    if (_currentIndex == index) {
        _currentIndex = -1;
        if (!_buttons.isEmpty())
            setCurrentIndex(0);
    } else if (_currentIndex > index) {
        --_currentIndex;
        // 同步 stacked widget
        _stackedWidget->getContainerStackedWidget()->setCurrentIndex(_currentIndex);
    }
}

void VerticalTabWidget::clear()
{
    while (count() > 0)
        removeTab(count() - 1);
}

void VerticalTabWidget::updateButtonSelection()
{
    for (int i = 0; i < _buttons.size(); ++i) {
        _buttons[i]->setProperty("active", i == _currentIndex);
        // 触发样式刷新
        _buttons[i]->style()->unpolish(_buttons[i]);
        _buttons[i]->style()->polish(_buttons[i]);
    }
}
