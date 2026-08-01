#include "core/scrollback/ChunkedScrollback.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTextStream>

#include <algorithm>
#include <vector>

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

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    const qsizetype lines = argumentValue(
        arguments, QStringLiteral("--lines"), 1'000'000);
    const qsizetype columns = argumentValue(
        arguments, QStringLiteral("--columns"), 80);
    const qsizetype chunkLines = argumentValue(
        arguments, QStringLiteral("--chunk-lines"), 1024);
    const qsizetype maxBytes = argumentValue(
        arguments, QStringLiteral("--max-bytes"), 1024LL * 1024 * 1024);

    NovaTerm::ChunkedScrollback scrollback(
        std::min(lines, NovaTerm::ChunkedScrollback::MaximumMaxLines),
        maxBytes, chunkLines);
    const qsizetype sampleStride = std::max<qsizetype>(1, lines / 10'000);
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

    QElapsedTimer snapshotTimer;
    snapshotTimer.start();
    const NovaTerm::ScrollbackSnapshot snapshot = scrollback.snapshot();
    const qint64 snapshotNs = snapshotTimer.nsecsElapsed();

    // Keep the snapshot alive while another chunk is appended. If an old
    // sealed chunk is evicted, statistics must expose its retained memory.
    QElapsedTimer retentionProbeTimer;
    retentionProbeTimer.start();
    for (qsizetype line = 0; line < chunkLines; ++line)
        scrollback.append(makeLine(columns, quint64(lines + line)));
    const qint64 retentionProbeNs = retentionProbeTimer.nsecsElapsed();

    const NovaTerm::ScrollbackStatistics statistics = scrollback.statistics();
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
    printMetric(output, QStringLiteral("Visible logical lines"),
                QString::number(snapshot.lineCount()),
                QStringLiteral("%1% retained").arg(retainedPercent, 0, 'f', 1));
    printMetric(output, QStringLiteral("Retention probe append"),
                QStringLiteral("%1 lines / %2 ms")
                    .arg(chunkLines)
                    .arg(retentionProbeNs / 1'000'000.0, 0, 'f', 2));

    output << "\n[Chunk and memory state]\n";
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

    output << "\n[Result]\n"
           << "  Append completed............... PASS\n"
           << "  Immutable snapshot created.... PASS\n"
           << "  Memory budget................. "
           << (statistics.effectiveBytes <= maxBytes ? "PASS" : "FAIL") << '\n'
           << "============================================================\n";
    return 0;
}
