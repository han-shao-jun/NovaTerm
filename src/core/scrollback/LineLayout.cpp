/**
 * @file   LineLayout.cpp
 * @brief  逻辑行折行与历史重排实现。
 *
 * 详见 LineLayout.h 的接口说明。本文件实现：
 * - wrapLine：按列宽把一行 Cell 序列折成多个 DisplayLine，正确处理
 *   宽字符占两格、宽字符延续格、以及"宽字符落在行尾恰好放不下"等情况；
 * - viewport：从 anchorLine 起向后扫描，依次折行并填满至 rowCount 行；
 * - ReflowEngine::Impl：worker 线程分批对整个快照执行 reflow，所有
 *   取消检查点都会尽快终止并发布 cancelled 批次。
 */
#include "LineLayout.h"

#include <QMetaObject>
#include <QString>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace NovaTerm {

QVector<DisplayLine> LineLayout::wrapLine(
    const LogicalLine& line, int columns,
    const std::function<bool()>& cancelled)
{
    QVector<DisplayLine> result;
    columns = std::max(1, columns);
    // 空行单独处理：仍需产出 1 个 DisplayLine 以占位。
    if (line.cells.isEmpty()) {
        result.push_back({line.id, 0, 0, 0, line.hardBreak});
        return result;
    }

    qsizetype start = 0;
    qsizetype wrap = 0;
    while (start < line.cells.size()) {
        qsizetype end = start;
        int used = 0;
        // 在一行内填充 Cell，直到塞满 columns 列或行尾。
        while (end < line.cells.size()) {
            // 每 256 个 Cell 检查一次取消，避免长行卡住 worker。
            if ((end & 0xff) == 0 && cancelled && cancelled())
                return {};
            const Cell& cell = line.cells[end];
            // 宽字符延续格：跳过它，它必然属于前一个宽 Cell。
            if (cell.isWideContinuation()) {
                ++end;
                continue;
            }
            const int width = std::clamp(int(cell.width), 1, 2);
            // 当前行已用列数 + 该字宽度超过列宽：在该字前断行。
            if (used > 0 && used + width > columns)
                break;
            // 比单列视口还宽的字形仍占一列，避免无限循环。
            used += width;
            ++end;
            // 宽字形必然带一个延续格，一并消费以保持原子性。
            if (width == 2 && end < line.cells.size()
                && line.cells[end].isWideContinuation()) {
                ++end;
            }
            if (used >= columns)
                break;
        }
        // 防御：极端情况（cell.width 异常）下保证每行至少前进 1 个 Cell。
        if (end <= start)
            end = start + 1;
        result.push_back({line.id, start, end, wrap++, false});
        start = end;
    }
    // 仅最后一行携带 hardBreak 标志，其余折行都是软换行。
    result.last().hardBreak = line.hardBreak;
    return result;
}

ViewportSnapshot LineLayout::viewport(const ScrollbackSnapshot& snapshot,
                                      LineId anchorLine, qsizetype wrapOffset,
                                      int columns, qsizetype rowCount,
                                      qsizetype trailingCache,
                                      quint64 generation)
{
    ViewportSnapshot result;
    result.sourceVersion = snapshot.version();
    result.generation = generation;
    result.columns = std::max(1, columns);
    if (snapshot.empty() || rowCount <= 0)
        return result;

    // 把 anchorLine 折算为行号；未命中时按 ID 落在快照区间之前/之后
    // 退化为首行/末行。
    qsizetype row = snapshot.rowForLineId(anchorLine);
    if (row < 0)
        row = anchorLine < snapshot.firstLineId() ? 0
                                                  : snapshot.lineCount() - 1;
    const qsizetype wanted = rowCount + std::max<qsizetype>(0, trailingCache);
    for (; row < snapshot.lineCount() && result.rows.size() < wanted; ++row) {
        const LogicalLine* logical = snapshot.lineAt(row);
        if (!logical)
            break;
        QVector<DisplayLine> wrapped = wrapLine(*logical, result.columns);
        // anchorLine 起始行可能从 wrapOffset 开始（用于精确还原滚动位置），
        // 其他行从 wrapIndex=0 开始。
        qsizetype first = logical->id == anchorLine
            ? std::clamp<qsizetype>(wrapOffset, 0,
                                    std::max<qsizetype>(0, wrapped.size() - 1))
            : 0;
        for (; first < wrapped.size() && result.rows.size() < wanted; ++first)
            result.rows.push_back(wrapped[first]);
    }
    return result;
}

class ReflowEngine::Impl
{
public:
    struct Request
    {
        ScrollbackSnapshot snapshot;
        int columns{0};
        quint64 generation{0};
        qsizetype batchLines{1024};
    };

    explicit Impl(ReflowEngine* owner) : owner(owner)
    {
        worker = std::thread([this]() { run(); });
    }

    ~Impl()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping.store(true);
            pending.reset();
        }
        // 取消所有可能代际，唤醒 worker 后立即退出。
        cancelledGeneration.store(std::numeric_limits<quint64>::max());
        changed.notify_one();
        if (worker.joinable())
            worker.join();
    }

    void submit(Request value)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            const quint64 latest = latestGeneration.load();
            // 旧 generation 请求直接丢弃，保证 pending 中始终是最新请求。
            if (latest != 0 && value.generation <= latest)
                return;
            latestGeneration.store(value.generation);
            pending = std::move(value);
        }
        changed.notify_one();
    }

    void cancel(quint64 generation)
    {
        quint64 current = cancelledGeneration.load();
        while (current < generation
               && !cancelledGeneration.compare_exchange_weak(current,
                                                               generation)) {}
    }

    bool isCancelled(quint64 generation) const
    {
        return stopping.load()
            || generation < latestGeneration.load()
            || (generation > 0
                && cancelledGeneration.load() >= generation);
    }

    void emitBatch(ReflowBatch batch)
    {
        QMetaObject::invokeMethod(owner,
        [target = owner, batch = std::move(batch)]() {
            emit target->batchReady(batch);
        }, Qt::QueuedConnection);
    }

    // worker 主循环：取出请求后分批扫描整个快照。每批结束时检查取消，
    // 命中则发布 cancelled 批次并终止本次 reflow。所有异常都被捕获
    // 并转换为 error 批次，保证 worker 不会因异常死锁 submit 调用方。
    void run()
    {
        for (;;) {
            Request request;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [this]() {
                    return stopping.load() || pending.has_value();
                });
                if (stopping.load())
                    return;
                request = std::move(*pending);
                pending.reset();
            }
            try {
              qsizetype physicalRows = 0;
              for (qsizetype start = 0;
                   start < request.snapshot.lineCount();) {
                const qsizetype end = std::min(
                    request.snapshot.lineCount(), start + request.batchLines);
                QVector<DisplayLine> batchRows;
                for (qsizetype row = start; row < end; ++row) {
                    if (isCancelled(request.generation)) {
                        emitBatch({request.snapshot.version(),
                                   request.generation, start, row - start,
                                   physicalRows, std::move(batchRows),
                                   false, true});
                        break;
                    }
                    const LogicalLine* line = request.snapshot.lineAt(row);
                    if (line) {
                        // 限制取消检查点之间的最长耗时：拒绝异常长的
                        // 逻辑行，避免在对象析构时阻塞数百万行的展开。
                        if (line->cells.size() > 4 * 1024 * 1024) {
                            throw std::length_error(
                                "logical line exceeds 4M-cell reflow limit");
                        }
                        QVector<DisplayLine> wrapped = LineLayout::wrapLine(
                            *line, request.columns, [this, &request]() {
                                return isCancelled(request.generation);
                            });
                        if (isCancelled(request.generation)) {
                            emitBatch({request.snapshot.version(),
                              request.generation, start, row - start,
                              physicalRows, std::move(batchRows), false, true});
                            break;
                        }
                        physicalRows += wrapped.size();
                        batchRows += std::move(wrapped);
                    }
                }
                if (isCancelled(request.generation))
                    break;
                emitBatch({request.snapshot.version(), request.generation,
                           start, end - start, physicalRows,
                           std::move(batchRows),
                           end == request.snapshot.lineCount(), false});
                start = end;
              }
            // 空快照也需要发布一次 completed=true 的批次，让 UI 收到结束信号。
            if (request.snapshot.empty() && !isCancelled(request.generation))
                emitBatch({request.snapshot.version(), request.generation,
                           0, 0, 0, {}, true, false});
            } catch (const std::exception& exception) {
                emitBatch({request.snapshot.version(), request.generation,
                           0, 0, 0, {}, true, false,
                           QString::fromUtf8(exception.what())});
            } catch (...) {
                emitBatch({request.snapshot.version(), request.generation,
                           0, 0, 0, {}, true, false,
                           QStringLiteral("unknown reflow worker failure")});
            }
        }
    }

    ReflowEngine* owner;
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<Request> pending;
    std::atomic<quint64> cancelledGeneration{0};
    std::atomic<quint64> latestGeneration{0};
    std::atomic<bool> stopping{false};
    std::thread worker;
};

ReflowEngine::ReflowEngine(QObject* parent)
    : QObject(parent), _impl(std::make_unique<Impl>(this))
{
    qRegisterMetaType<ReflowBatch>();
}

ReflowEngine::~ReflowEngine() = default;

void ReflowEngine::request(ScrollbackSnapshot snapshot, int columns,
                           quint64 generation, qsizetype batchLines)
{
    _impl->submit({std::move(snapshot), std::max(1, columns), generation,
                   std::max<qsizetype>(1, batchLines)});
}

void ReflowEngine::cancel(quint64 generation)
{
    _impl->cancel(generation);
}

} // namespace NovaTerm
