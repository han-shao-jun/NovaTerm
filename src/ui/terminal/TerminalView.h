#pragma once
#include <QWidget>
#include <QEvent>

class ITransport;
class QTermWidget;

// 包装一个 QTermWidget，将字节数据统一通过 ITransport 接口送入：
//
//   • 本地 — LocalShellTransport（Unix: posix_openpt + fork，Windows: ConPTY）
//   • 远程 — SSH / Serial / Telnet 传输实现
//
// 两者均走同一条路径：键盘 → ITransport::write / 终端输出 ← ITransport::readyRead。
// startLocalShell() 内部创建 LocalShellTransport 并通过 attachTransport() 桥接，
// 不再区分"本地 KPty / 远程 transport"两套机制。
//
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

    // ── 本地终端：通过 LocalShellTransport 驱动真实 shell ──
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

private:
    void applyThemeColorScheme();
    void setupContextMenu(const QPoint& pos);
    bool eventFilter(QObject* obj, QEvent* event) override;

    QTermWidget* _terminal{nullptr};
    ITransport* _transport{nullptr};
    bool _isLocalShell{false};
};
