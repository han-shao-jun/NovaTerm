/**
 * @file   TerminalView.h
 * @brief  终端视图：TerminalCore + TerminalRenderer + ITransport 组合。
 *
 * 通过 libvterm 仿真引擎 + QRhi GPU 渲染 + ITransport 接口统一桥接本地/远程
 * 终端数据通路。本地和远程均走同一条路径，不再区分两套机制。
 */
#pragma once
#include <QWidget>
#include <QEvent>
#include <QPointer>
#include "core/search/SearchEngine.h"
#include "session/LocalShellProfile.h"

class ITransport;
class TerminalCore;
class TerminalRenderer;
class TerminalColorScheme;
class QTimer;
class ElaLineEdit;
class TerminalSession;

// 终端视图：组合 TerminalCore（libvterm 仿真引擎）+ TerminalRenderer（QRhi GPU
// 渲染）+ TerminalSession（Transport 编排），通过 ITransport 接口统一桥接
// 本地/远程终端数据通路。
//
// 上行（用户输入 → 远端）：
//   键盘 → TerminalRenderer::keyPressEvent
//        → TerminalCore::processKeyPress()          [入有界命令队列]
//        → Parser Worker：VTAdapter 编码为终端字节
//        → TerminalCore::outputData 信号            [queued 回 GUI 线程]
//        → TerminalSession 转发 → ITransport::write()
//
// 下行（远端 → 屏幕）：
//   PTY/shell/网络输出 → ITransport::readyRead 信号
//        → SessionInputPump                         [64 KiB 分片 + pending 暂存]
//        → TerminalCore::writeInput()               [8 MiB BoundedByteQueue]
//        → Parser Worker：VTAdapter → libvterm → ScreenBuffer / Scrollback
//        → TerminalCore::damage 信号                [批量合并，queued 回 GUI 线程]
//        → RenderScheduler 按目标刷新率节流
//        → frameRequested → TerminalRenderer::update()
//
// 两个方向都不在 GUI 线程内解析：libvterm 只在 Parser Worker 中运行，View 拿到
// 的始终是值语义 Snapshot。背压由 SessionInputPump 与 Core 的高低水位闭环负责，
// View 不再持有未入队的 Transport 字节。
//
// 本地和远程均走同一条路径，不再区分"本地 KPty / 远程 transport"两套机制。
//
// 会话所有权：每个 TerminalView 自建并拥有一个 TerminalSession（_ownsSession
// 默认 true），全程管理其生命周期（1 View : 1 Session）。这是本项目采纳的
// 架构；原 P6 设想的 "SessionManager 拥有 Session、View 只非 owning attach"
// 已放弃，SessionManager 类已移除。详见
// docs/architecture/stages/P6_Session_and_Transport.md。
//
class TerminalView : public QWidget
{
    Q_OBJECT
public:
    // 本地 Shell 类型。会话对话框（SessionPage）中由用户选择，决定 Windows
    // 下启动哪个 shell：Cmd 关联到 Clink（chrisant996/clink，增强版 cmd），
    // PowerShell 启动 powershell.exe，Wsl 启动用户选择的发行版。Unix 下忽略
    // 该值（始终走平台默认 shell）。显式固定数值以兼容已经保存的历史配置。
    enum class LocalShellType { Cmd = 0, PowerShell = 1, Wsl = 2 };

    explicit TerminalView(QWidget* parent = nullptr);
    explicit TerminalView(TerminalSession* session, QWidget* parent = nullptr);
    ~TerminalView() override;

    // ── 本地终端：通过 LocalShellTransport 驱动真实 shell ──
    /**
     * @brief 启动本地 shell（按类型选择 cmd+Clink、PowerShell 或 WSL）。
     * @param type 本地 Shell 类型。
     * @param wslDistribution WSL 发行版名称；仅 Wsl 类型使用。
     */
    void startLocalShell(LocalShellType type = LocalShellType::Cmd);
    /** @brief 启动指定 WSL 发行版，或按类型启动其他本地 Shell。 */
    void startLocalShell(LocalShellType type,
                         const QString& wslDistribution);
    void startLocalShell(LocalShellType type, const QString& wslDistribution,
                         const QString& workingDirectory);
    void startLocalShell(const LocalShellConfig& config); ///< 按完整配置启动本地 shell
    void stopLocalShell();                                ///< 停止本地 shell
    bool isLocalShell() const { return _isLocalShell; }  ///< 是否为本地 shell 会话

    // ── 远程终端：ITransport 数据桥接（SSH/串口/Telnet）──
    /**
     * @brief 附加传输层（SSH/串口/Telnet），建立数据桥接。
     * @param transport 传输层。
     */
    void attachTransport(ITransport* transport);
    void detachTransport();                              ///< 分离传输层
    ITransport* transport() const;                        ///< 获取当前传输层
    TerminalSession* session() const;                     ///< 获取会话对象
    [[nodiscard]] bool ownsSession() const noexcept { return _ownsSession; } ///< 是否拥有会话所有权

    /** @brief 将文本按终端粘贴语义发送，并把输入焦点还给终端。 */
    void pasteText(const QString& text);
    /** @brief 粘贴文本后发送回车，用于执行由界面生成的终端命令。 */
    void submitText(const QString& text);
    /** @brief 请求交互式 Shell 上报当前工作目录。 */
    void requestWorkingDirectory();

    // ── 渲染器访问（替代原来的 terminalWidget()）─────────────
    TerminalRenderer* renderer() const { return _renderer; } ///< 获取渲染器

signals:
    void titleChanged(const QString& title);  ///< 标题变更（终端转义序列触发）
    void workingDirectoryReported(const QString& path); ///< Shell 当前目录已上报
    void workingDirectoryRequestFailed();              ///< Shell 当前目录查询超时
    void activityDetected();                   ///< 检测到终端活动
    void shellFinished();                      ///< shell 已退出

private:
    void applyThemeColorScheme();
    void retranslateUi();
    void setupContextMenu(const QPoint& pos);
    bool eventFilter(QObject* obj, QEvent* event) override;
    void showSearch();
    void hideSearch();

    TerminalCore*     _core{nullptr};
    TerminalRenderer* _renderer{nullptr};
    QPointer<TerminalSession> _session;
    ITransport*       _localTransport{nullptr};
    ITransport*       _displayTransport{nullptr};
    bool              _isLocalShell{false};
    bool              _ownsSession{true};

    // PTY 尺寸变更去抖定时器：拖动窗口时密集的 resize 事件合并为一次
    // SIGWINCH，避免 shell 被连续重绘请求轰击产生输出风暴。
    QTimer*           _resizeDebounce{nullptr};
    ElaLineEdit*      _searchLine{nullptr};
    quint64           _searchGeneration{0};
    QString           _lastTerminalTitle;
    QString           _workingDirectoryMarker;
    quint64           _workingDirectoryRequestGeneration{0};
    bool              _workingDirectoryRequestPending{false};
    int               _latestResizeColumns{80};
    int               _latestResizeRows{24};

    static constexpr int WorkingDirectoryRequestTimeoutMs = 3000;
};
