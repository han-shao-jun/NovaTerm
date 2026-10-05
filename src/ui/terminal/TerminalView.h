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
#include "session/transfer/SerialTransferTypes.h"

class ITransport;
class TerminalCore;
class TerminalRenderer;
struct TerminalColorScheme;
class QTimer;
class ElaLineEdit;
class ElaScrollBar;
class TerminalSession;
class SerialFileTransferController;
class SerialFileTransferDialog;

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
     * @return true 表示已交给会话 adopt；false 表示附加失败，transport
     *         的所有权仍留在调用方，由调用方负责回收。
     */
    /**
     * @brief 附着传输并设置接收 LF 的解析兼容模式。
     * @param transport 待附着的传输，成功后由 Session 接管。
     * @param lfImpliesCr 串口按配置传入；其他传输保持默认 false。
     * @return 成功附着返回 true；解析模式命令入队失败时返回 false，
     *         调用方仍拥有 transport。
     */
    bool attachTransport(ITransport* transport, bool lfImpliesCr = false);
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
    /** @brief 获取终端右侧的历史滚动条（无历史时隐藏，但始终存在）。 */
    ElaScrollBar* scrollBar() const { return _scrollBar; }

signals:
    void titleChanged(const QString& title);  ///< 标题变更（终端转义序列触发）
    void workingDirectoryReported(const QString& path); ///< Shell 当前目录已上报
    void workingDirectoryRequestFailed();              ///< Shell 当前目录查询超时
    void activityDetected();                   ///< 检测到终端活动
    void shellFinished();                      ///< shell 已退出

private:
    bool _serialReconnectPromptActive{false}; ///< 当前是否在追加串口重连等待点
    void applyColorScheme();
    void retranslateUi();
    void setupContextMenu(const QPoint& pos);
    /** @brief 获取当前串口传输活动状态，同时绑定输入门禁的观察指针。 */
    [[nodiscard]] bool fileTransferActive();
    void showFileTransfer(NovaTerm::FileTransfer::Direction direction);
    /** @brief 右键菜单与鼠标中键共用的系统剪贴板粘贴入口。 */
    void pasteFromClipboard();
    bool eventFilter(QObject* obj, QEvent* event) override;
    void showSearch();
    void hideSearch();
    /**
     * @brief 把渲染器发布的滚动状态映射到右侧滚动条。
     * @param maximumOffset 渲染器允许的最大回看偏移（历史显示行数）。
     * @param offset        当前回看偏移，0 表示实时底部。
     * @note  渲染器的偏移以"底部为 0、向上为正"计，与滚动条"顶部为 0、
     *        向下为正"相反，故 value = maximumOffset - offset。
     */
    void syncScrollBar(int maximumOffset, int offset);

    TerminalCore*     _core{nullptr};
    TerminalRenderer* _renderer{nullptr};
    QPointer<TerminalSession> _session;
    QPointer<SerialFileTransferController> _serialFileTransfer;
    QPointer<SerialFileTransferDialog> _transferDialog;
    int _transferWheelAccum{0}; ///< 活动传输时只用于本地滚动的滚轮余量
    ITransport*       _localTransport{nullptr};
    ITransport*       _displayTransport{nullptr};
    bool              _isLocalShell{false};
    bool              _ownsSession{true};
    bool              _colorSchemeUpdatePending{false};

    // PTY 尺寸变更去抖定时器：拖动窗口时密集的 resize 事件合并为一次
    // SIGWINCH，避免 shell 被连续重绘请求轰击产生输出风暴。
    QTimer*           _resizeDebounce{nullptr};
    ElaLineEdit*      _searchLine{nullptr};
    ElaScrollBar*     _scrollBar{nullptr};
    // 由 syncScrollBar() 写入滚动条时置位，使 valueChanged 不再反向驱动
    // 渲染器 —— 否则两边互相触发会形成回环。
    bool              _scrollBarSyncing{false};
    quint64           _searchGeneration{0};
    QString           _lastTerminalTitle;
    QString           _workingDirectoryMarker;
    quint64           _workingDirectoryRequestGeneration{0};
    bool              _workingDirectoryRequestPending{false};
    int               _latestResizeColumns{80};
    int               _latestResizeRows{24};

    static constexpr int WorkingDirectoryRequestTimeoutMs = 3000;
};
