/**
 * @file   SystemInformationDialog.h
 * @brief  SSH 远端系统详细信息窗口。
 */
#pragma once

#include <ElaDialog.h>
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

private:
    void showStatus(const QString& text);

    QString _sessionName;
    QByteArray _lastOutput;
    bool _pending{false};
    QVBoxLayout* _contentLayout{nullptr};
};
