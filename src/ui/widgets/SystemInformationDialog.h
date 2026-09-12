/**
 * @file   SystemInformationDialog.h
 * @brief  SSH 远端系统详细信息窗口。
 */
#pragma once

#include <ElaDialog.h>
class QResizeEvent;
class QScrollArea;
class QVBoxLayout;

/**
 * @brief 以卡片表格展示当前 SSH 主机的系统、硬件与资源信息。
 */
class SystemInformationDialog final : public ElaDialog
{
    Q_OBJECT

public:
    explicit SystemInformationDialog(const QString& sessionName,
                                     QWidget* parent);
    /**
     * @brief 展示面板共享的本地计算结果，不重复发起远端查询。
     * @param output  面板已汇总的信息行；为空表示首批尚未到达。
     * @param pending 详情预取尚未完成；缺失的卡片显示"采集中"而非"No data"。
     */
    void populate(const QByteArray& output, bool pending = false);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void showStatus(const QString& text);
    /**
     * @brief 按视口宽度固定滚动内容的高度。
     *
     * 卡片里的文字标签开了 word-wrap，QLabel 对换行文本的 `minimumSizeHint()`
     * 按极窄宽度估算，会把布局最小高度抬到远超实际内容；QScrollArea 用该值决定
     * 内容高度，就会出现"能滚到空白页"的多余行程。这里改用
     * `heightForWidth(视口宽度)` 算出真实需要的高度。
     */
    void updateContentHeight();

    QString _sessionName;
    QByteArray _lastOutput;
    bool _pending{false};
    QScrollArea* _scroll{nullptr};
    QVBoxLayout* _contentLayout{nullptr};
};
