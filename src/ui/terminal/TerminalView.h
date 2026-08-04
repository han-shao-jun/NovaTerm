#pragma once
#include <QWidget>
#include <QEvent>
#include "core/search/SearchEngine.h"

class ITransport;
class TerminalCore;
class TerminalRenderer;
class TerminalColorScheme;
class QTimer;
class QLineEdit;
class SessionInputPump;

// 终端视图：组合 TerminalCore（libvterm 仿真引擎）+ TerminalRenderer（QRhi GPU 渲染），
// 通过 ITransport 接口统一桥接本地/远程终端数据通路。
//
// 数据流：
//   键盘 → TerminalRenderer → TerminalCore::processKeyPress()
//        → TerminalCore::outputData 信号 → ITransport::write()
//
//   PTY/shell 输出 → ITransport::readyRead 信号
//        → TerminalCore::writeInput() → vterm_input_write()
//        → libvterm 解析 → VTermScreenCallbacks → TerminalRenderer::update()
//
// 本地和远程均走同一条路径，不再区分"本地 KPty / 远程 transport"两套机制。
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

    // ── 渲染器访问（替代原来的 terminalWidget()）─────────────
    TerminalRenderer* renderer() const { return _renderer; }

signals:
    void titleChanged(const QString& title);
    void activityDetected();
    void shellFinished();

private:
    void applyThemeColorScheme();
    void setupContextMenu(const QPoint& pos);
    bool eventFilter(QObject* obj, QEvent* event) override;
    void showSearch();
    void hideSearch();

    TerminalCore*     _core{nullptr};
    TerminalRenderer* _renderer{nullptr};
    ITransport*       _transport{nullptr};
    SessionInputPump* _inputPump{nullptr};
    bool              _isLocalShell{false};

    // PTY 尺寸变更去抖定时器：拖动窗口时密集的 resize 事件合并为一次
    // SIGWINCH，避免 shell 被连续重绘请求轰击产生输出风暴。
    QTimer*           _resizeDebounce{nullptr};
    QLineEdit*        _searchLine{nullptr};
    quint64           _searchGeneration{0};
};
