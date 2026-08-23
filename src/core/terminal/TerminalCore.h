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

#include "BoundedByteQueue.h"
#include "ScreenBuffer.h"
#include "core/scrollback/LineLayout.h"
#include "core/search/SearchEngine.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QObject>
#include <QString>

#include <memory>

class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

// 终端核心的 Qt facade。线程安全：传输字节与控制命令排队后由
// 专用工作线程独占消费 VTAdapter。
class TerminalCore : public QObject
{
    Q_OBJECT

public:
    // 一次输入写入的结果。backpressured=true 时调用方应暂停后续写入。
    struct InputWriteResult
    {
        qsizetype requestedBytes{0};  // 调用方请求写入的字节数
        qsizetype acceptedBytes{0};  // 实际进入队列的字节数
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
    void processMousePress(QMouseEvent* event);
    void processMouseMove(QMouseEvent* event);
    void processMouseRelease(QMouseEvent* event);
    void processWheel(QWheelEvent* event);
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
    NovaTerm::TerminalSnapshot snapshot() const;

    /**
     * @brief 构造渲染层专用稀疏快照。
     * @param dirtyRows 标记哪些 widget 行需要重新拷贝（true）或仅更新身份（false）。
     * @param scrollLine 视口向上滚动的历史行数；0 表示底部活动屏幕。
     * @param anchorLine 锚定历史行 ID（与 anchorWrap 配合实现精确滚动恢复）。
     * @param anchorWrap 锚定行内的 wrap 偏移。
     * @return 渲染快照，包含可见行 Cell 与行身份指纹。
     */
    NovaTerm::RendererSnapshot rendererSnapshot(
        const QVector<bool>& dirtyRows, int scrollLine,
        NovaTerm::LineId anchorLine = 0,
        qsizetype anchorWrap = 0) const;
    quint64 modelRevision() const;
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
    NovaTerm::ScrollbackStatistics scrollbackStatistics() const;

    /**
     * @brief 异步搜索滚动历史。结果通过 searchResultsReady 信号分批返回。
     */
    void searchScrollback(NovaTerm::SearchRequest request);
    void cancelSearch(quint64 generation);

    /**
     * @brief 请求滚动历史的 reflow（按新列数重新换行）。
     *        结果通过 reflowBatchReady 信号分批返回。
     * @param columns 新列数。
     * @param generation 生成代号；新请求会取消同 generation 的旧请求。
     * @param batchLines 每批处理的逻辑行数（默认 1024）。
     */
    void requestScrollbackReflow(int columns, quint64 generation,
                                 qsizetype batchLines = 1024);
    void cancelScrollbackReflow(quint64 generation);

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
    // revision 标识产生此半开 damage 区域的不可变模型发布。
    // 渲染器据此检测快照比已收到的 damage 更新，保守重建整帧。
    void damage(const NovaTerm::DirtyRegion& region, quint64 revision);
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
    class Runtime;
    std::unique_ptr<Runtime> _runtime;
    std::unique_ptr<NovaTerm::SearchEngine> _searchEngine;
    std::unique_ptr<NovaTerm::ReflowEngine> _reflowEngine;
};
