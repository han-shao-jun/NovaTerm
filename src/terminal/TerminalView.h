#pragma once
#include <QWidget>

class ITransport;
class QTermWidget;
#ifdef _WIN32
class WinConPty;
#endif

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
    // 本地 Shell 类型。会话对话框（SessionPage）中由用户选择，决定 Windows
    // 下启动哪个 shell：Cmd 关联到 Clink（chrisant996/clink，增强版 cmd），
    // PowerShell 则启动 powershell.exe。Unix 下忽略该值（始终走默认 shell）。
    enum class LocalShellType { Cmd, PowerShell };

    explicit TerminalView(QWidget* parent = nullptr);
    ~TerminalView() override;

    // ── 本地终端：QTermWidget 内置 KPty（Windows：ConPTY / Unix：pty）──
    void startLocalShell(LocalShellType type = LocalShellType::Cmd);
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

    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void applyThemeColorScheme();
    void setupContextMenu(const QPoint& pos);

    QTermWidget* _terminal{nullptr};
    ITransport* _transport{nullptr};
    bool _isLocalShell{false};
#ifdef _WIN32
    WinConPty*  _winPty{nullptr};
#endif
};
