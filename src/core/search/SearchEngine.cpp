/**
 * @file   SearchEngine.cpp
 * @brief  滚动历史全文搜索引擎实现。
 *
 * 详见 SearchEngine.h 的接口说明。本文件实现：
 * - 把 LogicalLine 的 Cell 序列转换为可被 QRegularExpression 匹配的 QString，
 *   并维护 utf16 偏移 → Cell 索引的映射，用于把命中区间还原为 Cell 区间；
 * - 拒绝嵌套量词等可能导致灾难性回溯的正则形态；
 * - 在 worker 线程中分批扫描，每批命中通过 QueuedConnection 投递给 GUI 线程。
 */
#include "SearchEngine.h"

#include <QMetaObject>
#include <QRegularExpression>
#include <QString>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

namespace NovaTerm {
namespace {

// 搜索模式最大长度，防止构造异常大的正则。
constexpr isize MaximumPatternLength = 16 * 1024;
// 单行最大可搜索字符数，超过则跳过该行，避免恶意输出撑爆 QString。
constexpr isize MaximumSearchLineCharacters = 4 * 1024 * 1024;

// 一行经预处理的可搜索数据：text 是供正则匹配的字符串，
// utf16ToCell 把 text 中每个 UTF-16 码元映射回原 Cell 索引，
// 用于把命中区间还原为 Cell 区间以便 UI 高亮。
struct SearchableLine
{
    QString text;
    std::vector<isize> utf16ToCell;
};

// 把一行 Cell 转换为可搜索字符串。cancelled 回调用于在转换过程中
// 响应取消请求（每 256 个 Cell 检查一次）。返回 nullopt 表示行过长
// 或被取消。
template <typename Cancelled>
std::optional<SearchableLine> makeSearchable(const LogicalLine& line,
                                              Cancelled cancelled)
{
    if (line.cells.size() > MaximumSearchLineCharacters)
        return std::nullopt;
    SearchableLine result;
    result.text.reserve(line.cells.size());
    result.utf16ToCell.reserve(line.cells.size() + 1);
    for (isize cellIndex = 0; cellIndex < line.cells.size(); ++cellIndex) {
        // 每 256 个 Cell 检查一次取消，平衡检查开销与响应延迟。
        if ((cellIndex & 0xff) == 0 && cancelled())
            return std::nullopt;
        const Cell& cell = line.cells[cellIndex];
        // 宽字符的延续格直接跳过：它属于前一个宽 Cell。
        if (cell.isWideContinuation())
            continue;
        for (uint32_t codepoint : cell.chars) {
            if (codepoint == 0 || codepoint == WideCharContinuation)
                break;
            const char32_t character = char32_t(codepoint);
            const QString encoded = QString::fromUcs4(&character, 1);
            if (result.text.size() + encoded.size()
                > MaximumSearchLineCharacters) {
                return std::nullopt;
            }
            result.text += encoded;
            // 同一个 Cell 的多个码元都映射到该 Cell 索引。
            for (isize i = 0; i < encoded.size(); ++i)
                result.utf16ToCell.push_back(cellIndex);
        }
    }
    // 哨兵：text 末尾对应行尾之后的 Cell 索引。
    result.utf16ToCell.push_back(isize(line.cells.size()));
    return result;
}

// 启发式检测可能导致灾难性回溯的正则形态：嵌套量词组
// （如 (a+)+ ）。Qt/PCRE2 内部也有回溯限制，但显式拒绝此形态
// 可让 worker 终止有确定上界，跨 PCRE2 构建行为一致。
bool potentiallyUnboundedRegex(const QString& expression)
{
    static const QRegularExpression nestedQuantifier(
        QStringLiteral(R"(\([^)]*[+*][^)]*\)\s*[+*{])"));
    return nestedQuantifier.match(expression).hasMatch();
}

// 把 UTF-16 命中区间转换为 Cell 区间。clamp 防止边界越界，
// 末尾的退化处理保证 endCell > startCell，避免空高亮。
SearchMatch toMatch(LineId lineId, const SearchableLine& line,
                    isize start, isize length)
{
    const isize boundedStart = std::clamp<isize>(
        start, 0, isize(line.utf16ToCell.size()) - 1);
    const isize boundedEnd = std::clamp<isize>(
        start + std::max<isize>(1, length), 0,
        isize(line.utf16ToCell.size()) - 1);
    const isize startCell = line.utf16ToCell[boundedStart];
    isize endCell = line.utf16ToCell[boundedEnd];
    if (boundedEnd > boundedStart && endCell <= startCell)
        endCell = startCell + 1;
    return {lineId, startCell, endCell};
}

} // namespace

// 搜索引擎私有实现：持有 worker 线程、待处理任务队列与代际取消状态。
class SearchEngine::Impl
{
public:
    struct Work
    {
        ScrollbackSnapshot snapshot;
        SearchRequest request;
    };

    explicit Impl(SearchEngine* owner) : owner(owner)
    {
        worker = std::thread([this]() { run(); });
    }

    ~Impl()
    {
        stopping.store(true);
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending.reset();
        }
        // 取消所有可能的代际，确保 worker 在 changed 上被唤醒后立即退出。
        cancelledGeneration.store(std::numeric_limits<u64>::max());
        changed.notify_one();
        if (worker.joinable())
            worker.join();
    }

    // 提交新搜索：把 generation-1 视为可取消代际上限，使仍在跑的上一代
    // 搜索在下一个检查点终止；同时用 latestGeneration 拒绝乱序到达的
    // 旧 generation 请求，保证 pending 中始终是最新搜索。
    void submit(Work work)
    {
        const u64 previous = work.request.generation > 0
            ? work.request.generation - 1 : 0;
        u64 current = cancelledGeneration.load();
        while (current < previous
               && !cancelledGeneration.compare_exchange_weak(current,
                                                               previous)) {}
        {
            std::lock_guard<std::mutex> lock(mutex);
            const u64 latest = latestGeneration.load();
            if (latest != 0 && work.request.generation <= latest)
                return;
            latestGeneration.store(work.request.generation);
            pending = std::move(work);
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

    // 判断指定 generation 是否已被取代或取消。
    bool cancelled(u64 generation) const
    {
        return stopping.load()
            || generation < latestGeneration.load()
            || (generation > 0
                && cancelledGeneration.load() >= generation);
    }

    void publish(SearchBatch batch)
    {
        QMetaObject::invokeMethod(owner,
        [target = owner, batch = std::move(batch)]() {
            emit target->resultsReady(batch);
        }, Qt::QueuedConnection);
    }

    void publishMatches(SearchBatch& batch)
    {
        SearchBatch emitted;
        emitted.generation = batch.generation;
        emitted.sourceVersion = batch.sourceVersion;
        emitted.scannedLines = batch.scannedLines;
        emitted.totalLines = batch.totalLines;
        emitted.matches = std::move(batch.matches);
        publish(std::move(emitted));
    }

    // 执行一次完整搜索：校验请求 → 构造正则 → 在 [startRow,endRow) 区间
    // 逐行扫描，命中累积到 batchSize 即发布一批，扫完或命中达到上限后
    // 发布最终批次（completed=true）。所有检查点都查询 cancelled 以
    // 便尽快响应取消。
    void runSearch(const Work& work)
    {
        SearchBatch batch;
        batch.generation = work.request.generation;
        batch.sourceVersion = work.snapshot.version();
        batch.totalLines = work.snapshot.lineCount();

        if (work.request.query.empty()) {
            batch.completed = true;
            publish(std::move(batch));
            return;
        }
        if (isize(work.request.query.size()) > MaximumPatternLength) {
            batch.completed = true;
            batch.error = "search pattern exceeds 16 KiB limit";
            publish(std::move(batch));
            return;
        }

        // query 现为 UTF-8 std::string；阶段 5 抽离匹配器前，这里仍用
        // QRegularExpression，在边界做一次 UTF-8→QString 转换。
        const QString query = QString::fromStdString(work.request.query);
        // 非正则模式：对 query 整体做正则转义，使特殊字符按字面匹配。
        QString expression = work.request.regularExpression
            ? query
            : QRegularExpression::escape(query);
        if (work.request.regularExpression
            && potentiallyUnboundedRegex(expression)) {
            batch.completed = true;
            batch.error = "regular expression rejected: nested quantifier";
            publish(std::move(batch));
            return;
        }
        if (work.request.wholeWord)
            expression = QStringLiteral("\\b(?:%1)\\b").arg(expression);
        QRegularExpression::PatternOptions options;
        if (!work.request.caseSensitive)
            options |= QRegularExpression::CaseInsensitiveOption;
        QRegularExpression regex(expression, options);
        if (!regex.isValid()) {
            batch.completed = true;
            batch.error = regex.errorString().toStdString();
            publish(std::move(batch));
            return;
        }

        // 把 firstLine/lastLine 的逻辑行 ID 折算为行号 rowForLineId。
        // 未命中（返回 -1）时按 ID 落在快照区间之前/之后二分到 0/endRow。
        isize startRow = 0;
        isize endRow = work.snapshot.lineCount();
        if (work.request.firstLine != 0) {
            const isize value = work.snapshot.rowForLineId(
                work.request.firstLine);
            startRow = value < 0 ? (work.request.firstLine
                    < work.snapshot.firstLineId() ? 0 : endRow) : value;
        }
        if (work.request.lastLine != 0) {
            const isize value = work.snapshot.rowForLineId(
                work.request.lastLine);
            endRow = value < 0 ? (work.request.lastLine
                    < work.snapshot.firstLineId() ? 0 : endRow) : value + 1;
        }
        endRow = std::max(startRow, endRow);
        batch.totalLines = endRow - startRow;
        const isize batchSize = std::max<isize>(
            1, work.request.resultBatchSize);
        const isize maximumResults = std::max<isize>(
            0, work.request.maximumResults);
        isize resultCount = 0;

        for (isize row = startRow; row < endRow; ++row) {
            if (cancelled(work.request.generation)) {
                batch.cancelled = true;
                batch.scannedLines = row - startRow;
                publish(std::move(batch));
                return;
            }
            const LogicalLine* logical = work.snapshot.lineAt(row);
            if (!logical)
                continue;
            // makeSearchable 失败可能是取消导致，需再次确认。
            const auto searchable = makeSearchable(*logical, [this, &work]() {
                return cancelled(work.request.generation);
            });
            if (!searchable) {
                if (cancelled(work.request.generation)) {
                    batch.cancelled = true;
                    publish(std::move(batch));
                    return;
                }
                continue;
            }
            const SearchableLine& line = *searchable;
            QRegularExpressionMatchIterator matches = regex.globalMatch(line.text);
            while (matches.hasNext() && resultCount < maximumResults) {
                if (cancelled(work.request.generation)) {
                    batch.cancelled = true;
                    publish(std::move(batch));
                    return;
                }
                const QRegularExpressionMatch match = matches.next();
                if (!match.hasMatch())
                    continue;
                batch.matches.push_back(toMatch(
                    logical->id, line, match.capturedStart(),
                    match.capturedLength()));
                ++resultCount;
                // 命中累积到 batchSize 即发布一批并清空，避免内存堆积。
                if (isize(batch.matches.size()) >= batchSize) {
                    batch.scannedLines = row - startRow + 1;
                    publishMatches(batch);
                }
            }
            if (resultCount >= maximumResults)
                break;
            batch.scannedLines = row - startRow + 1;
        }
        batch.completed = true;
        publish(std::move(batch));
    }

    // worker 线程主循环：阻塞等待 pending 任务，取出后执行 runSearch。
    // 任何异常都被捕获并转换为 error 批次发布，保证 worker 不会因
    // 异常退出而死锁 submit 调用方。
    void run()
    {
        for (;;) {
            Work work;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [this]() {
                    return stopping.load() || pending.has_value();
                });
                if (stopping.load())
                    return;
                work = std::move(*pending);
                pending.reset();
            }
            try {
                runSearch(work);
            } catch (const std::exception& exception) {
                SearchBatch batch;
                batch.generation = work.request.generation;
                batch.sourceVersion = work.snapshot.version();
                batch.completed = true;
                batch.error = exception.what();
                publish(std::move(batch));
            } catch (...) {
                SearchBatch batch;
                batch.generation = work.request.generation;
                batch.sourceVersion = work.snapshot.version();
                batch.completed = true;
                batch.error = "unknown search worker failure";
                publish(std::move(batch));
            }
        }
    }

    SearchEngine* owner;
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<Work> pending;
    std::atomic<u64> cancelledGeneration{0};
    std::atomic<u64> latestGeneration{0};
    std::atomic<bool> stopping{false};
    std::thread worker;
};

SearchEngine::SearchEngine(QObject* parent)
    : QObject(parent), _impl(std::make_unique<Impl>(this))
{
    qRegisterMetaType<SearchBatch>();
}

SearchEngine::~SearchEngine() = default;

void SearchEngine::search(ScrollbackSnapshot snapshot, SearchRequest request)
{
    _impl->submit({std::move(snapshot), std::move(request)});
}

void SearchEngine::cancel(u64 generation)
{
    _impl->cancel(generation);
}

} // namespace NovaTerm
