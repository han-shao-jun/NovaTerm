#pragma once

#include <QWidget>

class QButtonGroup;
class QVBoxLayout;
class ElaCentralStackedWidget;
class ElaPushButton;

/// 左侧垂直选项卡 + 右侧堆栈页面的复合控件。
/// API 对齐 QTabWidget，但选项卡栏固定于左侧。
class VerticalTabWidget : public QWidget
{
    Q_OBJECT

public:
    explicit VerticalTabWidget(QWidget* parent = nullptr);

    /// 添加一个选项卡，返回索引。
    int addTab(QWidget* page, const QString& label);

    int count() const;
    int currentIndex() const;
    QWidget* widget(int index) const;
    QWidget* currentWidget() const;

    QString tabText(int index) const;
    void setTabText(int index, const QString& text);

    void removeTab(int index);
    void clear();

public slots:
    void setCurrentIndex(int index);

signals:
    void currentChanged(int index);

private:
    void updateButtonSelection();

    QButtonGroup*           _buttonGroup  = nullptr;
    ElaCentralStackedWidget* _stackedWidget = nullptr;
    QVBoxLayout*            _buttonLayout = nullptr;
    QList<ElaPushButton*>   _buttons;
    int                     _currentIndex = -1;
};
