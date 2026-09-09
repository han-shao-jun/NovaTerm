/**
 * @file   SystemInformationDialog.h
 * @brief  SSH 远端系统详细信息窗口。
 */
#pragma once

#include <ElaDialog.h>
#include <QPointer>

class QTimer;
class QVBoxLayout;
class SshTransport;

/**
 * @brief 以卡片表格展示当前 SSH 主机的系统、硬件与资源信息。
 */
class SystemInformationDialog final : public ElaDialog
{
    Q_OBJECT

public:
    explicit SystemInformationDialog(const QString& sessionName,
                                     SshTransport* transport,
                                     QWidget* parent);

private:
    void requestInformation();
    void handleCommandFinished(quint64 requestId,
                               const QByteArray& standardOutput,
                               const QByteArray& standardError,
                               const QString& errorMessage);
    void showStatus(const QString& text);
    void populate(const QByteArray& output);

    QPointer<SshTransport> _transport;
    QString _sessionName;
    QByteArray _lastOutput;
    QVBoxLayout* _contentLayout{nullptr};
    QTimer* _retryTimer{nullptr};
    quint64 _requestId{0};
    int _retryCount{0};

    static constexpr int MaximumSubmitRetries = 20;
};
