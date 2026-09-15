/**
 * @file   TerminalTabWidget.h
 * @brief  终端页面专用标签控件，提供标签级上下文菜单。
 */
#pragma once

#include <ElaTabWidget.h>
#include <QHash>
#include <QPoint>
#include <QPointer>
#include <QTimer>

class ElaTabBar;
class ElaIconButton;
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
    enum class ConnectionAction { Hidden, Disconnect, Reconnect };

    explicit TerminalTabWidget(QWidget* parent = nullptr);
    ~TerminalTabWidget() override;

    /**
     * @brief 设置指定终端标签的连接动作。
     * @param page 标签页对象。
     * @param action 隐藏、断开或重连。
     */
    void setTabConnectionAction(QWidget* page, ConnectionAction action);
    /** 设置标签标题，并把完整文本同步到 Tooltip。 */
    void setTabText(int index, const QString& text);

signals:
    /**
     * @brief 用户请求编辑指定标签对应的终端会话参数。
     * @param index 被右击的标签索引。
     */
    void editSessionRequested(int index);
    /** 请求断开指定标签的会话，但保留后续重连能力。 */
    void disconnectRequested(QWidget* page);
    /** 请求重新连接指定标签的会话。 */
    void reconnectRequested(QWidget* page);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void tabInserted(int index) override;

private:
    /** @brief 返回 ElaTabWidget 构造时安装的 Ela 标签栏。 */
    [[nodiscard]] ElaTabBar* elaTabBar() const noexcept;
    /** @brief 返回坐标所在的有效标签标题；关闭按钮与空白区域返回 -1。 */
    [[nodiscard]] int tabTitleAt(const QPoint& position) const;
    /** @brief 为鼠标当前悬浮的标签重新安排延迟切换。 */
    void scheduleHoverSwitch(const QPoint& position);
    /** @brief 取消尚未触发的悬浮切换。 */
    void cancelHoverSwitch();
    /** @brief 在关闭按钮左侧安装连接动作按钮。 */
    void installConnectionAction(int index);
    /** @brief 按当前语言和动作刷新单个按钮。 */
    void applyConnectionAction(ElaIconButton* button,
                               ConnectionAction action);
    /** @brief 刷新全部连接动作按钮的翻译。 */
    void retranslateConnectionActions();
    void showTabContextMenu(const QPoint& position);

    QHash<QWidget*, QPointer<ElaIconButton>> _connectionButtons;
    QHash<QWidget*, ConnectionAction> _connectionActions;
    QTimer _hoverSwitchTimer;
    int _pendingHoverIndex{-1};

    static constexpr int HoverSwitchDelayMs = 200;
    static constexpr int ConnectionButtonExtent = 22;
    static constexpr int TabHorizontalChrome = 70;
};
