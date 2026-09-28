/**
 * @file   TerminalCore.h
 * @brief  终端核心层 Qt facade（线程安全）。
 *
 * 把传输层字节与控制命令排队到专用工作线程，由该线程独占 VTAdapter
 * 与底层 ScreenBuffer / ScrollbackBuffer。GUI 线程通过 QObject 信号
 * 接收模型发布（damage / cursorMoved / scrollbackChanged 等）。
 *
 * 线程模型：
 *   GUI 线程：writeInput / processKeyPress / resize / snapshot 等公开 API
 *   工作线程：workerMain 循环 → adapter->writeInput / flushDamage
 *   同步：BoundedByteQueue + 命令队列 + modelMutex + completionMutex
 */
#pragma once
#include "PublishedTerminalState.h"

#include "BoundedByteQueue.h"
#include "ScreenBuffer.h"
#include "TerminalState.h"
#include "core/scrollback/LineLayout.h"
#include "core/search/SearchEngine.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QObject>
#include <QString>

#include <memory>
#include <optional>
#include <vector>

class QKeyEvent;
class QMouseEvent;

// 终端核心的 Qt facade。线程安全：传输字节与控制命令排队后由
// 专用工作线程独占消费 VTAdapter。
class TerminalCore : public QObject
{
    Q_OBJECT

public:
    // 一次输入写入的结果。backpressured=true 时调用方应暂停后续写入。
    struct InputWriteResult
    {
        NovaTerm::isize requestedBytes{0};  // 调用方请求写入的字节数
        NovaTerm::isize acceptedBytes{0};  // 实际进入队列的字节数
        bool backpressured{false};   // 是否已触发背压

        bool fullyAccepted() const
        {
            return acceptedBytes == requestedBytes;
        }
    };

    explicit TerminalCore(int cols, int rows, QObject* parent = nullptr);
    ~TerminalCore() override;

    /**
     * @brief 将传输字节写入解析队列（GUI 线程调用）。
     * @param data 待写入字节视图。
     * @return 写入结果；acceptedBytes < requestedBytes 表示队列已施加背压。
     */
    InputWriteResult writeInput(QByteArrayView data);

    // ── 输入事件（异步排队到工作线程）──
    void processKeyPress(QKeyEvent* event);
    void processTextInput(const QString& text,
                          Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    // 鼠标事件：row/col 为终端单元格坐标（屏幕行，非历史文档行），
    // 由渲染层用 widgetToScreenCell 换算；row < 0 表示不更新鼠标位置。
    void processMousePress(QMouseEvent* event, int row, int col);
    void processMouseMove(QMouseEvent* event, int row, int col);
    void processMouseRelease(QMouseEvent* event, int row, int col);
    // 滚轮作为鼠标按键 4/5 上报一次按下+释放；仅在鼠标跟踪开启时由
    // 渲染层调用。
    void processWheel(bool up, int row, int col,
                      Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    // Alternate Scroll：向备用屏程序发送 count 次 ↑/↓ 光标键。
    void sendAlternateScroll(bool up, int count);
    // 解析器侧模式的同步查询（原子缓存，不拿模型锁），供渲染层路由
    // 滚轮/鼠标事件。
    [[nodiscard]] NovaTerm::MouseTrackingMode mouseTrackingMode()
        const noexcept;
    [[nodiscard]] bool isAlternateScreen() const noexcept;
    /**
     * @brief 是否应答应用的 OSC 52 读取查询（读取本机剪贴板发回应用）。
     * @note  默认关闭：任何远端程序都能借此读走本机剪贴板（外泄向量），
     *        主流终端同样默认不自动应答。粘贴进远端应用用终端自身的
     *        Ctrl+Shift+V / 括号粘贴即可。由 terminal.osc52ClipboardRead
     *        配置接线。
     */
    void setClipboardQueryAnswerEnabled(bool enabled);
    void focusIn();
    void focusOut();
    void pasteText(const QString& text);

    /**
     * @brief 通知终端尺寸变更。会排队到工作线程，与字节流保持有序。
     */
    void resize(int cols, int rows);
    int columns() const;
    int rows() const;

    // ── 模型查询（持有 modelMutex，可由 GUI 线程调用）──
    bool getCell(int row, int col, NovaTerm::Cell& out) const;

    /**
     * @brief 活动屏幕第 row 行是否为上一行的软换行延续。
     * @param row 活动屏幕行号。越界返回 false。
     * @return true 表示该行续接上一行，其间没有真实换行。
     * @note  复制选区时据此决定行间是否插入 
；超宽输出被自动换行成的
     *        多个屏幕行属于同一逻辑行，不应插入换行。
     */
    [[nodiscard]] bool rowContinuation(int row) const;
    NovaTerm::TerminalSnapshot snapshot() const;
    /** @brief 在一次模型锁内读取有界文本、标题、模式、光标与 revision。 */
    [[nodiscard]] NovaTerm::TerminalState terminalState(
        NovaTerm::u64 sinceLineId = 0, std::size_t maxBytes = 65536,
        std::size_t maxLines = 256) const;
    /** @brief 尝试读取文本快照；模型正被写入时立即返回空，不等待 Parser。 */
    [[nodiscard]] std::optional<NovaTerm::TerminalState> tryTerminalState(
        NovaTerm::u64 sinceLineId = 0, std::size_t maxBytes = 65536,
        std::size_t maxLines = 256) const;
    /** @brief 非阻塞读取模型版本，供只读门面复用未变化的快照。 */
    [[nodiscard]] std::optional<NovaTerm::u64> tryModelRevision() const;
    /** @brief 返回最近发布物，并按最多 4 Hz 合并请求 Parser 刷新。 */
    [[nodiscard]] std::shared_ptr<const NovaTerm::PublishedTerminalState>
        requestPublishedTerminalState();
    [[nodiscard]] NovaTerm::PublishedContextStatistics
        publishedContextStatistics() const noexcept;

    /**
     * @brief 构造渲染层专用稀疏快照。
     * @param dirtyRows 标记哪些 widget 行需要重新拷贝（true）或仅更新身份（false）。
     * @param scrollLine 视口向上滚动的历史行数；0 表示底部活动屏幕。
     * @param anchorLine 锚定历史行 ID（与 anchorWrap 配合实现精确滚动恢复）。
     * @param anchorWrap 锚定行内的 wrap 偏移。
     * @param rendererContentRevision 渲染器已消费的最高内容 revision。非脏活动行
     *        仅当其 row revision 严格大于此值时才计算内容指纹，否则填 0——因为
     *        渲染器的 revision 补回检查对 row revision 不超过它的行走 revision 分支、
     *        根本不读指纹（`TerminalRenderer.cpp` render 的补回段）。传 0（默认）
     *        表示对所有非脏行计算指纹，与旧行为一致；需要按指纹比对全部行的路径
     *        （如 live-scroll 行槽位旋转后的 rowsNeedingRebuildAfterMapping）必须
     *        传 0。
     * @return 渲染快照，包含可见行 Cell 与行身份指纹。
     */
    NovaTerm::RendererSnapshot rendererSnapshot(
        const std::vector<bool>& dirtyRows, int scrollLine,
        NovaTerm::LineId anchorLine = 0,
        NovaTerm::isize anchorWrap = 0,
        NovaTerm::u64 rendererContentRevision = 0) const;
    NovaTerm::u64 modelRevision() const;
    NovaTerm::CursorState cursorState() const;
    void flushDamage();
    void setDefaultColors(const NovaTerm::TerminalColor& foreground,
                          const NovaTerm::TerminalColor& background);

    // ── 滚动历史 ──
    int scrollbackLineCount() const;
    bool getScrollbackCell(int lineIndex, int col, NovaTerm::Cell& out) const;
    void setScrollbackLimit(int lines);
    void clearScrollback();
    NovaTerm::ScrollbackSnapshot scrollbackSnapshot() const;

    /**
     * @brief 取滚动历史尾部增量视图（不封存分块、不复制 ChunkView）。
     * @param sinceId  渲染器上次已折进布局的最后一条逻辑行 ID。
     * @param maxLines 尾部深拷贝行数上界；超过则返回 resync=true。
     * @return 尾部增量视图。
     * @note  供渲染器每批 scrollbackChanged 增量维护显示布局，替代高频的
     *        全量 scrollbackSnapshot()，消除分块碎片化与 ChunkView churn。
     */
    NovaTerm::ScrollbackTail scrollbackTail(NovaTerm::LineId sinceId,
                                            NovaTerm::isize maxLines) const;

    NovaTerm::ScrollbackStatistics scrollbackStatistics() const;

    /**
     * @brief 异步搜索滚动历史。结果通过 searchResultsReady 信号分批返回。
     */
    void searchScrollback(NovaTerm::SearchRequest request);
    void cancelSearch(NovaTerm::u64 generation);

    /**
     * @brief 请求滚动历史的 reflow（按新列数重新换行）。
     *        结果通过 reflowBatchReady 信号分批返回。
     * @param columns 新列数。
     * @param generation 生成代号；新请求会取消同 generation 的旧请求。
     * @param batchLines 每批处理的逻辑行数（默认 1024）。
     */
    void requestScrollbackReflow(int columns, NovaTerm::u64 generation,
                                 NovaTerm::isize batchLines = 1024);
    void cancelScrollbackReflow(NovaTerm::u64 generation);

    NovaTerm::Position cursorPosition() const;
    bool cursorVisible() const;
    NovaTerm::CursorShape cursorShape() const;
    bool cursorBlink() const;
    QString title() const;

    // 测试/基准同步接口；生产渲染走信号驱动，不依赖此方法。
    bool waitForIdle(int timeoutMs = 5000) const;
    NovaTerm::BoundedByteQueue::Statistics queueStatistics() const;

signals:
    void outputData(const QByteArray& data);
    void titleChanged(const QString& title);
    void bell();
    // OSC 52 写剪贴板：mask 为 NovaTerm::Selection 位组合，text 为应用
    // 请求写入的解码后文本（空串表示清除）。生产路径由 TerminalCore 自行
    // 落到 QClipboard；信号供测试与自定义剪贴板策略监听。
    void clipboardWriteRequested(int mask, const QString& text);
    // revision 标识产生此半开 damage 区域的不可变模型发布。
    // 渲染器据此检测快照比已收到的 damage 更新，保守重建整帧。
    void damage(const NovaTerm::DirtyRegion& region, NovaTerm::u64 revision);
    void cursorMoved();
    void scrollbackChanged();
    // 本次发布的活动屏幕上滚行数。与 scrollbackChanged 分离，因为后者
    // 会被合并以减少信号风暴。
    void screenScrolled(int rows);
    void inputBackpressureChanged(bool paused);
    void inputOverload(const QString& reason);
    void searchResultsReady(const NovaTerm::SearchBatch& batch);
    void reflowBatchReady(const NovaTerm::ReflowBatch& batch);

private:
    /** @note 调用方必须持有 modelMutex。 */
    [[nodiscard]] NovaTerm::TerminalState terminalStateLocked(
        NovaTerm::u64 sinceLineId, std::size_t maxBytes, std::size_t maxLines) const;
    // OSC 52 的 GUI 线程落点（worker 回调经 QueuedConnection 切入）。
    void applySelectionToClipboard(int mask, const QString& text);
    void answerSelectionQuery(int mask);    class Runtime;
    std::unique_ptr<Runtime> _runtime;
    std::unique_ptr<NovaTerm::SearchEngine> _searchEngine;
    std::unique_ptr<NovaTerm::ReflowEngine> _reflowEngine;
};
