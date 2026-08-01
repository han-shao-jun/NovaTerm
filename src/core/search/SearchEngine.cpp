#include "SearchEngine.h"

#include <QMetaObject>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

namespace NovaTerm {
namespace {

constexpr qsizetype MaximumPatternLength = 16 * 1024;
constexpr qsizetype MaximumSearchLineCharacters = 4 * 1024 * 1024;

struct SearchableLine
{
    QString text;
    QVector<qsizetype> utf16ToCell;
};

SearchableLine makeSearchable(const LogicalLine& line)
{
    SearchableLine result;
    result.text.reserve(line.cells.size());
    result.utf16ToCell.reserve(line.cells.size() + 1);
    for (qsizetype cellIndex = 0; cellIndex < line.cells.size(); ++cellIndex) {
        const Cell& cell = line.cells[cellIndex];
        if (cell.isWideContinuation())
            continue;
        for (uint32_t codepoint : cell.chars) {
            if (codepoint == 0 || codepoint == WideCharContinuation)
                break;
            const char32_t character = char32_t(codepoint);
            const QString encoded = QString::fromUcs4(&character, 1);
            result.text += encoded;
            for (qsizetype i = 0; i < encoded.size(); ++i)
                result.utf16ToCell.push_back(cellIndex);
        }
    }
    result.utf16ToCell.push_back(line.cells.size());
    return result;
}

SearchMatch toMatch(LineId lineId, const SearchableLine& line,
                    qsizetype start, qsizetype length)
{
    const qsizetype boundedStart = std::clamp<qsizetype>(
        start, 0, line.utf16ToCell.size() - 1);
    const qsizetype boundedEnd = std::clamp<qsizetype>(
        start + std::max<qsizetype>(1, length), 0,
        line.utf16ToCell.size() - 1);
    const qsizetype startCell = line.utf16ToCell[boundedStart];
    qsizetype endCell = line.utf16ToCell[boundedEnd];
    if (boundedEnd > boundedStart && endCell <= startCell)
        endCell = startCell + 1;
    return {lineId, startCell, endCell};
}

} // namespace

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
        cancelledGeneration.store(std::numeric_limits<quint64>::max());
        changed.notify_one();
        if (worker.joinable())
            worker.join();
    }

    void submit(Work work)
    {
        const quint64 previous = work.request.generation > 0
            ? work.request.generation - 1 : 0;
        quint64 current = cancelledGeneration.load();
        while (current < previous
               && !cancelledGeneration.compare_exchange_weak(current,
                                                               previous)) {}
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending = std::move(work);
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

    bool cancelled(quint64 generation) const
    {
        return stopping.load()
            || (generation > 0
                && cancelledGeneration.load() >= generation);
    }

    void publish(SearchBatch batch)
    {
        QMetaObject::invokeMethod(owner, [target = owner, batch]() {
            emit target->resultsReady(batch);
        }, Qt::QueuedConnection);
    }

    void runSearch(const Work& work)
    {
        SearchBatch batch;
        batch.generation = work.request.generation;
        batch.sourceVersion = work.snapshot.version();
        batch.totalLines = work.snapshot.lineCount();

        if (work.request.query.isEmpty()) {
            batch.completed = true;
            publish(std::move(batch));
            return;
        }
        if (work.request.query.size() > MaximumPatternLength) {
            batch.completed = true;
            batch.error = QStringLiteral("search pattern exceeds 16 KiB limit");
            publish(std::move(batch));
            return;
        }

        QString expression = work.request.regularExpression
            ? work.request.query
            : QRegularExpression::escape(work.request.query);
        if (work.request.wholeWord)
            expression = QStringLiteral("\\b(?:%1)\\b").arg(expression);
        QRegularExpression::PatternOptions options;
        if (!work.request.caseSensitive)
            options |= QRegularExpression::CaseInsensitiveOption;
        QRegularExpression regex(expression, options);
        if (!regex.isValid()) {
            batch.completed = true;
            batch.error = regex.errorString();
            publish(std::move(batch));
            return;
        }

        qsizetype startRow = 0;
        qsizetype endRow = work.snapshot.lineCount();
        if (work.request.firstLine != 0) {
            const qsizetype value = work.snapshot.rowForLineId(
                work.request.firstLine);
            startRow = value < 0 ? (work.request.firstLine
                    < work.snapshot.firstLineId() ? 0 : endRow) : value;
        }
        if (work.request.lastLine != 0) {
            const qsizetype value = work.snapshot.rowForLineId(
                work.request.lastLine);
            endRow = value < 0 ? (work.request.lastLine
                    < work.snapshot.firstLineId() ? 0 : endRow) : value + 1;
        }
        endRow = std::max(startRow, endRow);
        batch.totalLines = endRow - startRow;
        const qsizetype batchSize = std::max<qsizetype>(
            1, work.request.resultBatchSize);
        const qsizetype maximumResults = std::max<qsizetype>(
            0, work.request.maximumResults);
        qsizetype resultCount = 0;

        for (qsizetype row = startRow; row < endRow; ++row) {
            if (cancelled(work.request.generation)) {
                batch.cancelled = true;
                batch.scannedLines = row - startRow;
                publish(std::move(batch));
                return;
            }
            const LogicalLine* logical = work.snapshot.lineAt(row);
            if (!logical)
                continue;
            const SearchableLine line = makeSearchable(*logical);
            if (line.text.size() > MaximumSearchLineCharacters)
                continue;
            QRegularExpressionMatchIterator matches = regex.globalMatch(line.text);
            while (matches.hasNext() && resultCount < maximumResults) {
                const QRegularExpressionMatch match = matches.next();
                if (!match.hasMatch())
                    continue;
                batch.matches.push_back(toMatch(
                    logical->id, line, match.capturedStart(),
                    match.capturedLength()));
                ++resultCount;
                if (batch.matches.size() >= batchSize) {
                    batch.scannedLines = row - startRow + 1;
                    publish(batch);
                    batch.matches.clear();
                }
            }
            if (resultCount >= maximumResults)
                break;
            batch.scannedLines = row - startRow + 1;
        }
        batch.completed = true;
        publish(std::move(batch));
    }

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
            runSearch(work);
        }
    }

    SearchEngine* owner;
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<Work> pending;
    std::atomic<quint64> cancelledGeneration{0};
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

void SearchEngine::cancel(quint64 generation)
{
    _impl->cancel(generation);
}

} // namespace NovaTerm
