#pragma once
#include <QWidget>

class ITransport;
class QTermWidget;

// 包装一个 QTermWidget，并将字节数据从两个互斥来源之一送入：
//   • 本地  — QTermWidget 内置的 KPty 驱动真实 shell（ConPTY/pty）。
//             数据不经过 ITransport。
//   • 远程 — 字节通过 ITransport 桥接（SSH/串口/Telnet）：
//             终端按键 -> transport->write，transport->readyRead
//             -> terminal->receiveData。
// startLocalShell() / attachTransport() 各自先分离对方模式，因此
// 始终只有一个来源处于活动状态。
class TerminalView : public QWidget
{
    Q_OBJECT
public:
    explicit TerminalView(QWidget* parent = nullptr);
    ~TerminalView() override;

    // ── 本地终端：QTermWidget 内置 KPty（Windows：ConPTY / Unix：pty）──
    void startLocalShell();
    void stopLocalShell();
    bool isLocalShell() const { return _isLocalShell; }

    // ── 远程终端：ITransport 数据桥接（SSH/串口/Telnet）──
    void attachTransport(ITransport* transport);
    void detachTransport();
    ITransport* transport() const { return _transport; }
    QTermWidget* terminalWidget() const { return _terminal; }

signals:
    void titleChanged(const QString& title);
    void activityDetected();
    void shellFinished();

private slots:
    void onTransportReadyRead(const QByteArray& data);
    void onTransportDisconnected();
    void onLocalShellFinished();

private:
    void setupContextMenu(const QPoint& pos);

    QTermWidget* _terminal{nullptr};
    ITransport* _transport{nullptr};
    bool _isLocalShell{false};
};
