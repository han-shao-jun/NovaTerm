/**
 * @file   TerminalTabWidget.h
 * @brief  终端页面专用标签控件，提供标签级上下文菜单。
 */
#pragma once

#include <ElaTabWidget.h>
#include <QPoint>
#include <QTimer>

class ElaTabBar;
class QEvent;

/**
 * @brief 终端页面专用标签控件。
 *
 * 负责标签命中与上下文菜单展示，不理解 TerminalView 或会话生命周期；具体操作
 * 通过标签索引信号交还 TerminalPage。
 */
class TerminalTabWidget final : public ElaTabWidget
{
    Q_OBJECT

public:
    explicit TerminalTabWidget(QWidget* parent = nullptr);

signals:
    /**
     * @brief 用户请求编辑指定标签对应的终端会话参数。
     * @param index 被右击的标签索引。
     */
    void editSessionRequested(int index);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /** @brief 返回 ElaTabWidget 构造时安装的 Ela 标签栏。 */
    [[nodiscard]] ElaTabBar* elaTabBar() const noexcept;
    /** @brief 返回坐标所在的有效标签标题；关闭按钮与空白区域返回 -1。 */
    [[nodiscard]] int tabTitleAt(const QPoint& position) const;
    /** @brief 为鼠标当前悬浮的标签重新安排延迟切换。 */
    void scheduleHoverSwitch(const QPoint& position);
    /** @brief 取消尚未触发的悬浮切换。 */
    void cancelHoverSwitch();
    void showTabContextMenu(const QPoint& position);

    QTimer _hoverSwitchTimer;
    int _pendingHoverIndex{-1};

    static constexpr int HoverSwitchDelayMs = 200;
};
