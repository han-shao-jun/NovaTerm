#include "core/scrollback/ChunkedScrollback.h"
#include "core/scrollback/LineLayout.h"
#include "core/search/SearchEngine.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QTimer>
#include <QTextStream>

#include <algorithm>
#include <vector>

#if defined(Q_OS_UNIX)
#include <sys/resource.h>
#endif
#if defined(Q_OS_LINUX)
#include <unistd.h>
#endif

namespace {

qsizetype argumentValue(const QStringList& arguments, const QString& name,
                        qsizetype fallback)
{
    const qsizetype index = arguments.indexOf(name);
    if (index < 0 || index + 1 >= arguments.size())
        return fallback;
    bool ok = false;
    const qlonglong value = arguments[index + 1].toLongLong(&ok);
    return ok && value > 0 ? qsizetype(value) : fallback;
}

NovaTerm::LogicalLine makeLine(qsizetype columns, quint64 number)
{
    NovaTerm::LogicalLine line;
    line.cells.resize(columns);
    for (qsizetype column = 0; column < columns; ++column) {
        line.cells[column].chars[0] = uint32_t('a' + (number + column) % 26);
        line.cells[column].width = 1;
    }
    return line;
}

double percentile(std::vector<qint64> samples, double quantile)
{
    if (samples.empty())
        return 0.0;
    std::sort(samples.begin(), samples.end());
    const size_t index = std::min(samples.size() - 1,
        size_t(quantile * double(samples.size() - 1)));
    return samples[index] / 1000.0;
}

double percent(qint64 value, qint64 total)
{
    return total > 0 ? 100.0 * double(value) / double(total) : 0.0;
}

void printMetric(QTextStream& output, const QString& name,
                 const QString& value, const QString& note = {})
{
    output << "  " << name.leftJustified(30, QLatin1Char('.')) << ' '
           << value.rightJustified(16);
    if (!note.isEmpty())
        output << "  " << note;
    output << '\n';
}

double peakResidentMiB()
{
#if defined(Q_OS_UNIX)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(Q_OS_MACOS)
        return usage.ru_maxrss / (1024.0 * 1024.0);
#else
        return usage.ru_maxrss / 1024.0;
#endif
    }
#endif
    return -1.0;
}

double currentResidentMiB()
{
#if defined(Q_OS_LINUX)
    QFile statm(QStringLiteral("/proc/self/statm"));
    if (statm.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> fields = statm.readAll().simplified().split(' ');
        bool ok = false;
        const qlonglong residentPages = fields.size() > 1
            ? fields[1].toLongLong(&ok) : 0;
        if (ok) {
            return residentPages * double(sysconf(_SC_PAGESIZE))
                / (1024.0 * 1024.0);
        }
    }
#endif
    return -1.0;
}

QString residentValue(double value)
{
    return value >= 0
        ? QStringLiteral("%1 MiB").arg(value, 0, 'f', 2)
        : QStringLiteral("unavailable");
}

QString residentDelta(double value, double baseline)
{
    if (value < 0 || baseline < 0)
        return {};
    const double delta = value - baseline;
    return QStringLiteral("%1%2 MiB vs baseline")
        .arg(delta >= 0 ? QStringLiteral("+") : QString())
        .arg(delta, 0, 'f', 2);
}

int runMatrix(const QCoreApplication& application,
              const QStringList& arguments)
{
    QVector<qsizetype> workloads{1'000, 10'000, 100'000, 1'000'000};
    const qsizetype listIndex = arguments.indexOf(
        QStringLiteral("--matrix-lines"));
    if (listIndex >= 0 && listIndex + 1 < arguments.size()) {
        QVector<qsizetype> parsed;
        for (const QString& item : arguments[listIndex + 1].split(',')) {
            bool ok = false;
            const qlonglong value = item.trimmed().toLongLong(&ok);
            if (ok && value > 0)
                parsed.push_back(qsizetype(value));
        }
        if (!parsed.isEmpty())
            workloads = std::move(parsed);
    }

    const qsizetype columns = argumentValue(
        arguments, QStringLiteral("--columns"), 80);
    const qsizetype chunkLines = argumentValue(
        arguments, QStringLiteral("--chunk-lines"), 1024);
    const qsizetype maxBytes = argumentValue(
        arguments, QStringLiteral("--max-bytes"), 256LL * 1024 * 1024);

    QTextStream output(stdout);
    output << "\n============================================================\n"
           << " NovaTerm P4 Benchmark Matrix - Independent Processes\n"
           << "============================================================\n"
           << "  Lines: ";
    for (qsizetype i = 0; i < workloads.size(); ++i) {
        if (i > 0)
            output << ", ";
        output << workloads[i];
    }
    output << "\n  Columns: " << columns
           << "\n  Chunk lines: " << chunkLines
           << "\n  Live memory budget: " << maxBytes / (1024.0 * 1024.0)
           << " MiB\n"
           << "  Each scenario runs in a fresh process so RSS values are isolated.\n"
           << "============================================================\n";
    output.flush();

    int failures = 0;
    for (const qsizetype lines : workloads) {
        QProcess child;
        child.setProgram(application.applicationFilePath());
        child.setArguments({QStringLiteral("--lines"), QString::number(lines),
                            QStringLiteral("--columns"), QString::number(columns),
                            QStringLiteral("--chunk-lines"), QString::number(chunkLines),
                            QStringLiteral("--max-bytes"), QString::number(maxBytes)});
        child.setProcessChannelMode(QProcess::ForwardedChannels);
        child.start();
        if (!child.waitForStarted(5'000) || !child.waitForFinished(-1)
            || child.exitStatus() != QProcess::NormalExit
            || child.exitCode() != 0) {
            ++failures;
        }
    }
    return failures == 0 ? 0 : 2;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.contains(QStringLiteral("--matrix")))
        return runMatrix(application, arguments);
    const qsizetype lines = argumentValue(
        arguments, QStringLiteral("--lines"), 1'000'000);
    const qsizetype columns = argumentValue(
        arguments, QStringLiteral("--columns"), 80);
    const qsizetype chunkLines = argumentValue(
        arguments, QStringLiteral("--chunk-lines"), 1024);
    const qsizetype maxBytes = argumentValue(
        arguments, QStringLiteral("--max-bytes"), 1024LL * 1024 * 1024);

    const double baselineResidentMiB = currentResidentMiB();
    NovaTerm::ChunkedScrollback scrollback(
        std::min(lines, NovaTerm::ChunkedScrollback::MaximumMaxLines),
        maxBytes, chunkLines);
    const qsizetype targetSamples = std::clamp<qsizetype>(
        lines / 10, 100, 10'000);
    const qsizetype sampleStride = std::max<qsizetype>(
        1, lines / targetSamples);
    std::vector<qint64> appendLatencySamples;
    appendLatencySamples.reserve(size_t(lines / sampleStride + 1));

    QElapsedTimer appendTimer;
    appendTimer.start();
    for (qsizetype line = 0; line < lines; ++line) {
        NovaTerm::LogicalLine logicalLine = makeLine(columns, quint64(line));
        if (line % sampleStride == 0) {
            QElapsedTimer sampleTimer;
            sampleTimer.start();
            scrollback.append(std::move(logicalLine));
            appendLatencySamples.push_back(sampleTimer.nsecsElapsed());
        } else {
            scrollback.append(std::move(logicalLine));
        }
    }
    const qint64 appendNs = appendTimer.nsecsElapsed();
    const double appendResidentMiB = currentResidentMiB();

    QElapsedTimer snapshotTimer;
    snapshotTimer.start();
    const NovaTerm::ScrollbackSnapshot snapshot = scrollback.snapshot();
    const qint64 snapshotNs = snapshotTimer.nsecsElapsed();
    const double snapshotResidentMiB = currentResidentMiB();

    constexpr qsizetype lookupSamples = 100'000;
    qsizetype lookupHits = 0;
    QElapsedTimer lookupTimer;
    lookupTimer.start();
    for (qsizetype i = 0; i < lookupSamples && !snapshot.empty(); ++i) {
        const NovaTerm::LineId id = snapshot.firstLineId()
            + NovaTerm::LineId((i * snapshot.lineCount()) / lookupSamples);
        lookupHits += snapshot.lineById(id) != nullptr;
    }
    const qint64 lookupNs = lookupTimer.nsecsElapsed();

    // Keep the snapshot alive while another chunk is appended. If an old
    // sealed chunk is evicted, statistics must expose its retained memory.
    QElapsedTimer retentionProbeTimer;
    retentionProbeTimer.start();
    for (qsizetype line = 0; line < chunkLines; ++line)
        scrollback.append(makeLine(columns, quint64(lines + line)));
    const qint64 retentionProbeNs = retentionProbeTimer.nsecsElapsed();
    const double retentionResidentMiB = currentResidentMiB();

    const NovaTerm::ScrollbackStatistics statistics = scrollback.statistics();

    QElapsedTimer viewportTimer;
    viewportTimer.start();
    const NovaTerm::ViewportSnapshot viewport = NovaTerm::LineLayout::viewport(
        snapshot, snapshot.firstLineId(), 0, int(columns), 64, 32, 1);
    const qint64 viewportNs = viewportTimer.nsecsElapsed();

    NovaTerm::ReflowEngine reflow;
    QEventLoop reflowLoop;
    QTimer reflowTimeout;
    reflowTimeout.setSingleShot(true);
    qint64 reflowFirstBatchNs = -1;
    bool reflowCompleted = false;
    QElapsedTimer reflowTimer;
    QObject::connect(&reflow, &NovaTerm::ReflowEngine::batchReady,
                     &reflowLoop, [&](const NovaTerm::ReflowBatch& batch) {
        if (batch.generation != 1)
            return;
        if (reflowFirstBatchNs < 0)
            reflowFirstBatchNs = reflowTimer.nsecsElapsed();
        if (batch.completed || batch.cancelled || !batch.error.isEmpty()) {
            reflowCompleted = batch.completed && batch.error.isEmpty();
            reflowLoop.quit();
        }
    });
    QObject::connect(&reflowTimeout, &QTimer::timeout,
                     &reflowLoop, &QEventLoop::quit);
    reflowTimer.start();
    reflow.request(snapshot, int(columns), 1, 512);
    reflowTimeout.start(10'000);
    reflowLoop.exec();
    const qint64 reflowNs = reflowTimer.nsecsElapsed();

    NovaTerm::SearchEngine search;
    QEventLoop searchLoop;
    QTimer searchTimeout;
    searchTimeout.setSingleShot(true);
    qint64 searchFirstBatchNs = -1;
    qsizetype searchMatches = 0;
    bool searchCompleted = false;
    QElapsedTimer searchTimer;
    QObject::connect(&search, &NovaTerm::SearchEngine::resultsReady,
                     &searchLoop, [&](const NovaTerm::SearchBatch& batch) {
        if (batch.generation != 1)
            return;
        if (searchFirstBatchNs < 0)
            searchFirstBatchNs = searchTimer.nsecsElapsed();
        searchMatches += batch.matches.size();
        if (batch.completed || batch.cancelled || !batch.error.isEmpty()) {
            searchCompleted = batch.completed && batch.error.isEmpty();
            searchLoop.quit();
        }
    });
    QObject::connect(&searchTimeout, &QTimer::timeout,
                     &searchLoop, &QEventLoop::quit);
    NovaTerm::SearchRequest searchRequest;
    searchRequest.query = QStringLiteral("mnop");
    searchRequest.generation = 1;
    searchRequest.resultBatchSize = 256;
    searchTimer.start();
    search.search(snapshot, searchRequest);
    searchTimeout.start(10'000);
    searchLoop.exec();
    const qint64 searchNs = searchTimer.nsecsElapsed();

    QEventLoop cancelLoop;
    QTimer cancelTimeout;
    cancelTimeout.setSingleShot(true);
    bool cancellationObserved = false;
    QElapsedTimer cancelTimer;
    QObject::connect(&search, &NovaTerm::SearchEngine::resultsReady,
                     &cancelLoop, [&](const NovaTerm::SearchBatch& batch) {
        if (batch.generation == 2 && batch.cancelled) {
            cancellationObserved = true;
            cancelLoop.quit();
        }
    });
    QObject::connect(&cancelTimeout, &QTimer::timeout,
                     &cancelLoop, &QEventLoop::quit);
    NovaTerm::SearchRequest cancelRequest;
    cancelRequest.query = QStringLiteral("not present in generated rows");
    cancelRequest.generation = 2;
    cancelTimer.start();
    search.search(snapshot, cancelRequest);
    search.cancel(2);
    cancelTimeout.start(2'000);
    cancelLoop.exec();
    const qint64 cancelNs = cancelTimer.nsecsElapsed();
    const double asyncResidentMiB = currentResidentMiB();
    const double observedPeakMiB = std::max(
        {peakResidentMiB(), baselineResidentMiB, appendResidentMiB,
         snapshotResidentMiB, retentionResidentMiB, asyncResidentMiB});
    const double appendLinesPerSecond = appendNs > 0
        ? double(lines) * 1'000'000'000.0 / double(appendNs) : 0.0;
    const qint64 totalAppendedLines = qint64(lines) + qint64(chunkLines);
    const double retainedPercent = percent(snapshot.lineCount(), lines);
    const double budgetPercent = percent(statistics.effectiveBytes, maxBytes);

    QTextStream output(stdout);
    output.setRealNumberNotation(QTextStream::FixedNotation);
    output.setRealNumberPrecision(2);
    output << "\n============================================================\n"
           << " NovaTerm P4 Benchmark - Chunked Scrollback and Snapshot\n"
           << "============================================================\n"
           << "\n[Configuration]\n";
#ifdef NDEBUG
    printMetric(output, QStringLiteral("Build mode"), QStringLiteral("Release"));
#else
    printMetric(output, QStringLiteral("Build mode"), QStringLiteral("Debug / unoptimized"),
                QStringLiteral("use Release for baseline comparisons"));
#endif
    printMetric(output, QStringLiteral("Qt version"), QString::fromLatin1(qVersion()));

    output << "\n[Workload]\n";
    printMetric(output, QStringLiteral("Requested logical lines"), QString::number(lines));
    printMetric(output, QStringLiteral("Columns per line"), QString::number(columns));
    printMetric(output, QStringLiteral("Cells generated"),
                QString::number(qulonglong(lines) * qulonglong(columns)));
    printMetric(output, QStringLiteral("Chunk capacity"),
                QStringLiteral("%1 lines").arg(chunkLines));
    printMetric(output, QStringLiteral("Memory budget"),
                QStringLiteral("%1 MiB").arg(maxBytes / (1024.0 * 1024.0), 0, 'f', 2));

    output << "\n[Append performance]\n";
    printMetric(output, QStringLiteral("Total elapsed"),
                QStringLiteral("%1 ms").arg(appendNs / 1'000'000.0, 0, 'f', 2));
    printMetric(output, QStringLiteral("Throughput"),
                QStringLiteral("%1 lines/s").arg(appendLinesPerSecond, 0, 'f', 0));
    printMetric(output, QStringLiteral("Sampled append P50"),
                QStringLiteral("%1 us").arg(percentile(appendLatencySamples, 0.50), 0, 'f', 2));
    printMetric(output, QStringLiteral("Sampled append P99"),
                QStringLiteral("%1 us").arg(percentile(appendLatencySamples, 0.99), 0, 'f', 2));
    printMetric(output, QStringLiteral("Latency samples"),
                QString::number(appendLatencySamples.size()),
                QStringLiteral("stride %1").arg(sampleStride));

    output << "\n[Snapshot]\n";
    printMetric(output, QStringLiteral("Creation latency"),
                QStringLiteral("%1 us").arg(snapshotNs / 1000.0, 0, 'f', 2));
    printMetric(output, QStringLiteral("Snapshot version"),
                QString::number(statistics.version));
    printMetric(output, QStringLiteral("LineId lookup"),
                QStringLiteral("%1 ns/op")
                    .arg(lookupNs / double(std::max<qsizetype>(1, lookupSamples)),
                         0, 'f', 2),
                QStringLiteral("%1/%2 hits")
                    .arg(lookupHits).arg(lookupSamples));
    printMetric(output, QStringLiteral("Visible logical lines"),
                QString::number(snapshot.lineCount()),
                QStringLiteral("%1% retained").arg(retainedPercent, 0, 'f', 1));
    printMetric(output, QStringLiteral("Retention probe append"),
                QStringLiteral("%1 lines / %2 ms")
                    .arg(chunkLines)
                    .arg(retentionProbeNs / 1'000'000.0, 0, 'f', 2));

    output << "\n[Chunk and memory state]\n";
    printMetric(output, QStringLiteral("Process baseline RSS"),
                residentValue(baselineResidentMiB));
    printMetric(output, QStringLiteral("RSS after append"),
                residentValue(appendResidentMiB),
                residentDelta(appendResidentMiB, baselineResidentMiB));
    printMetric(output, QStringLiteral("RSS after snapshot"),
                residentValue(snapshotResidentMiB),
                residentDelta(snapshotResidentMiB, baselineResidentMiB));
    printMetric(output, QStringLiteral("RSS after retention probe"),
                residentValue(retentionResidentMiB),
                residentDelta(retentionResidentMiB, baselineResidentMiB));
    printMetric(output, QStringLiteral("RSS after async work"),
                residentValue(asyncResidentMiB),
                residentDelta(asyncResidentMiB, baselineResidentMiB));
    printMetric(output, QStringLiteral("Sealed chunks"),
                QString::number(statistics.sealedChunks));
    printMetric(output, QStringLiteral("Active tail lines"),
                QString::number(statistics.activeLines));
    printMetric(output, QStringLiteral("Logical cells retained"),
                QString::number(statistics.logicalCells));
    printMetric(output, QStringLiteral("Effective bytes"),
                QStringLiteral("%1 MiB").arg(statistics.effectiveBytes /
                                              (1024.0 * 1024.0), 0, 'f', 2),
                QStringLiteral("%1% of budget").arg(budgetPercent, 0, 'f', 1));
    printMetric(output, QStringLiteral("Snapshot-retained bytes"),
                QStringLiteral("%1 MiB").arg(statistics.retainedBySnapshots /
                                              (1024.0 * 1024.0), 0, 'f', 2),
                QStringLiteral("old chunks kept alive by snapshot"));
    printMetric(output, QStringLiteral("Live + retained bytes"),
                QStringLiteral("%1 MiB").arg(
                    (statistics.effectiveBytes + statistics.retainedBySnapshots) /
                    (1024.0 * 1024.0), 0, 'f', 2));
    printMetric(output, QStringLiteral("Observed peak RSS"),
                residentValue(observedPeakMiB),
                QStringLiteral("OS high-water mark"));

    output << "\n[Viewport, reflow and search]\n";
    printMetric(output, QStringLiteral("Viewport first frame"),
                QStringLiteral("%1 us / %2 rows")
                    .arg(viewportNs / 1000.0, 0, 'f', 2)
                    .arg(viewport.rows.size()));
    printMetric(output, QStringLiteral("Reflow first batch"),
                reflowFirstBatchNs >= 0
                    ? QStringLiteral("%1 ms").arg(reflowFirstBatchNs / 1e6, 0, 'f', 2)
                    : QStringLiteral("TIMEOUT"));
    printMetric(output, QStringLiteral("Full retained reflow"),
                QStringLiteral("%1 ms").arg(reflowNs / 1e6, 0, 'f', 2),
                reflowCompleted ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
    printMetric(output, QStringLiteral("Search first batch"),
                searchFirstBatchNs >= 0
                    ? QStringLiteral("%1 ms").arg(searchFirstBatchNs / 1e6, 0, 'f', 2)
                    : QStringLiteral("TIMEOUT"));
    printMetric(output, QStringLiteral("Search total"),
                QStringLiteral("%1 ms / %2 matches")
                    .arg(searchNs / 1e6, 0, 'f', 2).arg(searchMatches),
                searchCompleted ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
    printMetric(output, QStringLiteral("Search cancellation"),
                QStringLiteral("%1 ms").arg(cancelNs / 1e6, 0, 'f', 2),
                cancellationObserved ? QStringLiteral("PASS") : QStringLiteral("FAIL"));

    output << "\n[Eviction]\n";
    printMetric(output, QStringLiteral("Evicted lines"),
                QString::number(statistics.evictedLines),
                QStringLiteral("%1% of input").arg(percent(statistics.evictedLines,
                                                        totalAppendedLines), 0, 'f', 1));
    printMetric(output, QStringLiteral("Evicted chunks"),
                QString::number(statistics.evictedChunks));
    printMetric(output, QStringLiteral("Budget respected"),
                statistics.effectiveBytes <= maxBytes
                    ? QStringLiteral("PASS") : QStringLiteral("FAIL"));

    const bool passed = statistics.effectiveBytes <= maxBytes
        && !viewport.rows.isEmpty() && reflowCompleted && searchCompleted
        && cancellationObserved;
    output << "\n[Result]\n"
           << "  Append completed............... PASS\n"
           << "  Immutable snapshot created.... PASS\n"
           << "  Memory budget................. "
           << (statistics.effectiveBytes <= maxBytes ? "PASS" : "FAIL") << '\n'
           << "  Async reflow/search............ "
           << (reflowCompleted && searchCompleted && cancellationObserved
                   ? "PASS" : "FAIL") << '\n'
           << "============================================================\n";
    return passed ? 0 : 2;
}
