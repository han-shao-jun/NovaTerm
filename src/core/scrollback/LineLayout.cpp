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

#include "core/ThreadNaming.h"

#include <QMetaObject>

#include <algorithm>
#include <condition_variable>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace NovaTerm {

std::vector<DisplayLine> LineLayout::wrapLine(
    const LogicalLine& line, int columns,
    const std::function<bool()>& cancelled)
{
    std::vector<DisplayLine> result;
    columns = std::max(1, columns);
    // 空行单独处理：仍需产出 1 个 DisplayLine 以占位。
    if (line.cells.empty()) {
        result.push_back({line.id, 0, 0, 0, line.hardBreak});
        return result;
    }

    // 超长逻辑行在此截断。上限放在 wrapLine 而不是调用方，使每帧的
    // viewport 与 worker 上的 ReflowEngine 得到一致行为 —— 旧实现只在
    // ReflowEngine 里抛异常，渲染路径无上限。
    const isize cellLimit =
        std::min<isize>(line.cells.size(), MaxWrapCells);

    isize start = 0;
    isize wrap = 0;
    while (start < cellLimit) {
        isize end = start;
        int used = 0;
        // 在一行内填充 Cell，直到塞满 columns 列或行尾。
        while (end < cellLimit) {
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
            if (width == 2 && end < cellLimit
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
    result.back().hardBreak = line.hardBreak;
    return result;
}

ViewportSnapshot LineLayout::viewport(const ScrollbackSnapshot& snapshot,
                                      LineId anchorLine, isize wrapOffset,
                                      int columns, isize rowCount,
                                      isize trailingCache,
                                      u64 generation)
{
    ViewportSnapshot result;
    result.sourceVersion = snapshot.version();
    result.generation = generation;
    result.columns = std::max(1, columns);
    if (snapshot.empty() || rowCount <= 0)
        return result;

    // 把 anchorLine 折算为行号；未命中时按 ID 落在快照区间之前/之后
    // 退化为首行/末行。
    isize row = snapshot.rowForLineId(anchorLine);
    if (row < 0)
        row = anchorLine < snapshot.firstLineId() ? 0
                                                  : snapshot.lineCount() - 1;
    const isize wanted = rowCount + std::max<isize>(0, trailingCache);
    for (; row < snapshot.lineCount()
         && isize(result.rows.size()) < wanted; ++row) {
        const LogicalLine* logical = snapshot.lineAt(row);
        if (!logical)
            break;
        std::vector<DisplayLine> wrapped = wrapLine(*logical, result.columns);
        // anchorLine 起始行可能从 wrapOffset 开始（用于精确还原滚动位置），
        // 其他行从 wrapIndex=0 开始。
        isize first = logical->id == anchorLine
            ? std::clamp<isize>(wrapOffset, 0,
                                    std::max<isize>(0, isize(wrapped.size()) - 1))
            : 0;
        for (; first < isize(wrapped.size())
             && isize(result.rows.size()) < wanted; ++first)
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
        u64 generation{0};
        isize batchLines{1024};
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
        cancelledGeneration.store(std::numeric_limits<u64>::max());
        changed.notify_one();
        if (worker.joinable())
            worker.join();
    }

    void submit(Request value)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            const u64 latest = latestGeneration.load();
            // 旧 generation 请求直接丢弃，保证 pending 中始终是最新请求。
            if (latest != 0 && value.generation <= latest)
                return;
            latestGeneration.store(value.generation);
            pending = std::move(value);
        }
        changed.notify_one();
    }

    void cancel(u64 generation)
    {
        u64 current = cancelledGeneration.load();
        while (current < generation
               && !cancelledGeneration.compare_exchange_weak(current,
                                                               generation)) {}
    }

    bool isCancelled(u64 generation) const
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
        setCurrentThreadName("nvterm-reflow");
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
              isize physicalRows = 0;
              for (isize start = 0;
                   start < request.snapshot.lineCount();) {
                const isize end = std::min(
                    request.snapshot.lineCount(), start + request.batchLines);
                std::vector<DisplayLine> batchRows;
                for (isize row = start; row < end; ++row) {
                    if (isCancelled(request.generation)) {
                        emitBatch({request.snapshot.version(),
                                   request.generation, start, row - start,
                                   physicalRows, std::move(batchRows),
                                   false, true, {}});
                        break;
                    }
                    const LogicalLine* line = request.snapshot.lineAt(row);
                    if (line) {
                        // 超长逻辑行由 wrapLine 按 MaxWrapCells 截断，此处
                        // 不再抛异常 —— 旧写法会让整次 reflow 以错误批次
                        // 结束，渲染层收到后永不提交布局。
                        std::vector<DisplayLine> wrapped = LineLayout::wrapLine(
                            *line, request.columns, [this, &request]() {
                                return isCancelled(request.generation);
                            });
                        if (isCancelled(request.generation)) {
                            emitBatch({request.snapshot.version(),
                              request.generation, start, row - start,
                              physicalRows, std::move(batchRows),
                              false, true, {}});
                            break;
                        }
                        physicalRows += wrapped.size();
                        batchRows.insert(batchRows.end(),
                            std::make_move_iterator(wrapped.begin()),
                            std::make_move_iterator(wrapped.end()));
                    }
                }
                if (isCancelled(request.generation))
                    break;
                emitBatch({request.snapshot.version(), request.generation,
                           start, end - start, physicalRows,
                           std::move(batchRows),
                           end == request.snapshot.lineCount(), false, {}});
                start = end;
              }
            // 空快照也需要发布一次 completed=true 的批次，让 UI 收到结束信号。
            if (request.snapshot.empty() && !isCancelled(request.generation))
                emitBatch({request.snapshot.version(), request.generation,
                           0, 0, 0, {}, true, false, {}});
            } catch (const std::exception& exception) {
                emitBatch({request.snapshot.version(), request.generation,
                           0, 0, 0, {}, true, false,
                           exception.what()});
            } catch (...) {
                emitBatch({request.snapshot.version(), request.generation,
                           0, 0, 0, {}, true, false,
                           "unknown reflow worker failure"});
            }
        }
    }

    ReflowEngine* owner;
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<Request> pending;
    std::atomic<u64> cancelledGeneration{0};
    std::atomic<u64> latestGeneration{0};
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
                           u64 generation, isize batchLines)
{
    _impl->submit({std::move(snapshot), std::max(1, columns), generation,
                   std::max<isize>(1, batchLines)});
}

void ReflowEngine::cancel(u64 generation)
{
    _impl->cancel(generation);
}

} // namespace NovaTerm
