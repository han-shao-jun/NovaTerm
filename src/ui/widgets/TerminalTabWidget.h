/**
 * @file   TerminalTabWidget.h
 * @brief  终端页面专用标签控件，提供标签级上下文菜单。
 */
#pragma once

#include <ElaTabWidget.h>
#include <QPoint>

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

private:
    void showTabContextMenu(const QPoint& position);
};
