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
    if (line.cells.isEmpty()) {
        result.push_back({line.id, 0, 0, 0, line.hardBreak});
        return result;
    }

    qsizetype start = 0;
    qsizetype wrap = 0;
    while (start < line.cells.size()) {
        qsizetype end = start;
        int used = 0;
        while (end < line.cells.size()) {
            if ((end & 0xff) == 0 && cancelled && cancelled())
                return {};
            const Cell& cell = line.cells[end];
            if (cell.isWideContinuation()) {
                ++end;
                continue;
            }
            const int width = std::clamp(int(cell.width), 1, 2);
            if (used > 0 && used + width > columns)
                break;
            // A glyph wider than a one-column viewport still occupies one row.
            used += width;
            ++end;
            if (width == 2 && end < line.cells.size()
                && line.cells[end].isWideContinuation()) {
                ++end;
            }
            if (used >= columns)
                break;
        }
        if (end <= start)
            end = start + 1;
        result.push_back({line.id, start, end, wrap++, false});
        start = end;
    }
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
                        // Bound time spent between cancellation points. Very
                        // large lines are rejected instead of blocking object
                        // destruction while materialising millions of rows.
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
