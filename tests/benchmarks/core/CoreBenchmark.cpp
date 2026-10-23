#include "core/terminal/TerminalCore.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTextStream>
#include <QThread>

#include <algorithm>
#include <vector>

namespace {

constexpr double ParserTargetMiBPerSecond = 20.0;

struct SubmissionMetrics
{
    quint64 retries{0};
    qint64 backpressureWaitNanoseconds{0};
    std::vector<qint64> batchLatencyNanoseconds;
};

qint64 argumentValue(const QStringList& arguments, const QString& name, qint64 fallback)
{
    const qsizetype index = arguments.indexOf(name);
    if (index < 0 || index + 1 >= arguments.size())
        return fallback;

    bool ok = false;
    const qint64 value = arguments[index + 1].toLongLong(&ok);
    return ok && value > 0 ? value : fallback;
}

double mibPerSecond(qint64 bytes, qint64 nanoseconds)
{
    if (nanoseconds <= 0)
        return 0.0;
    return (double(bytes) / (1024.0 * 1024.0))
        / (double(nanoseconds) / 1'000'000'000.0);
}

quint64 enqueueWithoutLoss(TerminalCore& core, const QByteArray& data,
                           SubmissionMetrics* metrics = nullptr)
{
    QElapsedTimer batchTimer;
    if (metrics)
        batchTimer.start();
    quint64 retries = 0;
    qsizetype offset = 0;
    while (offset < data.size()) {
        const TerminalCore::InputWriteResult result =
            core.writeInput(QByteArrayView(data).sliced(offset));
        offset += result.acceptedBytes;
        if (result.fullyAccepted())
            continue;
        ++retries;
        // Model a paused transport instead of hammering the queue mutex while
        // the parser is draining it. The production InputPump resumes from the
        // low-watermark signal; this short backoff provides the same property
        // in a benchmark that does not run the Qt event loop.
        QElapsedTimer waitTimer;
        if (metrics)
            waitTimer.start();
        QThread::usleep(50);
        if (metrics)
            metrics->backpressureWaitNanoseconds += waitTimer.nsecsElapsed();
    }
    if (metrics) {
        metrics->retries += retries;
        metrics->batchLatencyNanoseconds.push_back(batchTimer.nsecsElapsed());
    }
    return retries;
}

double percent(qint64 value, qint64 total)
{
    return total > 0 ? 100.0 * double(value) / double(total) : 0.0;
}

double percentileMilliseconds(std::vector<qint64> samples, double quantile)
{
    if (samples.empty())
        return 0.0;
    std::sort(samples.begin(), samples.end());
    const size_t index = std::min(samples.size() - 1,
        size_t(quantile * double(samples.size() - 1)));
    return samples[index] / 1'000'000.0;
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
    const qint64 targetBytes = argumentValue(arguments, QStringLiteral("--bytes"),
                                             20LL * 1024 * 1024);
    const qint64 targetLines = argumentValue(arguments, QStringLiteral("--lines"), 100'000);

    TerminalCore core(120, 40);
    core.setScrollbackLimit(int(std::min<qint64>(targetLines, 1'000'000)));

    const QByteArray parserChunk =
        QByteArrayLiteral("\x1b[38;5;42mNovaTerm\x1b[0m parser baseline ")
        + QString::fromUtf8(u8"中文宽字符").toUtf8()
        + QByteArrayLiteral("\r\n");
    QByteArray parserBatch;
    parserBatch.reserve(64 * 1024);
    while (parserBatch.size() + parserChunk.size() <= 64 * 1024)
        parserBatch.append(parserChunk);

    QElapsedTimer parserTimer;
    parserTimer.start();
    qint64 parsedBytes = 0;
    quint64 parserRetries = 0;
    SubmissionMetrics submissionMetrics;
    while (parsedBytes < targetBytes) {
        parserRetries += enqueueWithoutLoss(core, parserBatch,
                                            &submissionMetrics);
        parsedBytes += parserBatch.size();
    }
    const qint64 parserSubmissionNanoseconds = parserTimer.nsecsElapsed();
    if (!core.waitForIdle(60'000)) {
        QTextStream(stderr) << "ERROR: parser did not become idle within 60 s\n";
        return 2;
    }
    const qint64 parserNanoseconds = parserTimer.nsecsElapsed();
    const NovaTerm::BoundedByteQueue::Statistics parserQueue =
        core.queueStatistics();

    const QByteArray scrollbackLine = QByteArrayLiteral("scrollback baseline line\r\n");
    QElapsedTimer scrollbackTimer;
    scrollbackTimer.start();
    quint64 scrollbackRetries = 0;
    for (qint64 line = 0; line < targetLines; ++line)
        scrollbackRetries += enqueueWithoutLoss(core, scrollbackLine);
    if (!core.waitForIdle(60'000)) {
        QTextStream(stderr) << "ERROR: scrollback workload did not become idle within 60 s\n";
        return 3;
    }
    const qint64 scrollbackNanoseconds = scrollbackTimer.nsecsElapsed();
    const NovaTerm::BoundedByteQueue::Statistics finalQueue =
        core.queueStatistics();

    const double parserThroughput = mibPerSecond(parsedBytes, parserNanoseconds);
    const double scrollbackLinesPerSecond = scrollbackNanoseconds > 0
        ? double(targetLines) * 1'000'000'000.0 / double(scrollbackNanoseconds)
        : 0.0;
    const bool parserTargetMet = parserThroughput >= ParserTargetMiBPerSecond;

    QTextStream output(stdout);
    output.setRealNumberNotation(QTextStream::FixedNotation);
    output.setRealNumberPrecision(2);
    output << "\n============================================================\n"
           << " NovaTerm P2 Benchmark - Async Parser and Backpressure\n"
           << "============================================================\n"
           << "\n[Configuration]\n";
#ifdef NDEBUG
    printMetric(output, QStringLiteral("Build mode"), QStringLiteral("Release"));
#else
    printMetric(output, QStringLiteral("Build mode"), QStringLiteral("Debug / unoptimized"),
                QStringLiteral("use Release for baseline comparisons"));
#endif
    printMetric(output, QStringLiteral("Qt version"), QString::fromLatin1(qVersion()));
    printMetric(output, QStringLiteral("Terminal geometry"), QStringLiteral("120 x 40"));

    output << "\n[Parser pipeline]\n";
    printMetric(output, QStringLiteral("Input"),
                QStringLiteral("%1 MiB").arg(parsedBytes / (1024.0 * 1024.0), 0, 'f', 2));
    printMetric(output, QStringLiteral("Transport batch"),
                QStringLiteral("%1 KiB").arg(parserBatch.size() / 1024.0, 0, 'f', 2));
    printMetric(output, QStringLiteral("Elapsed"),
                QStringLiteral("%1 ms").arg(parserNanoseconds / 1'000'000.0, 0, 'f', 2));
    printMetric(output, QStringLiteral("Throughput"),
                QStringLiteral("%1 MiB/s").arg(parserThroughput, 0, 'f', 2),
                QStringLiteral("target >= %1 MiB/s").arg(ParserTargetMiBPerSecond, 0, 'f', 0));
    printMetric(output, QStringLiteral("Target status"),
                parserTargetMet ? QStringLiteral("PASS") : QStringLiteral("BELOW TARGET"));

    output << "\n[Transport submission and latency]\n";
    printMetric(output, QStringLiteral("Submitted batches"),
                QString::number(submissionMetrics.batchLatencyNanoseconds.size()));
    printMetric(output, QStringLiteral("Submission elapsed"),
                QStringLiteral("%1 ms").arg(
                    parserSubmissionNanoseconds / 1'000'000.0, 0, 'f', 2));
    printMetric(output, QStringLiteral("Final drain latency"),
                QStringLiteral("%1 ms").arg(
                    (parserNanoseconds - parserSubmissionNanoseconds) /
                    1'000'000.0, 0, 'f', 2));
    printMetric(output, QStringLiteral("Batch latency P50"),
                QStringLiteral("%1 ms").arg(percentileMilliseconds(
                    submissionMetrics.batchLatencyNanoseconds, 0.50), 0, 'f', 3));
    printMetric(output, QStringLiteral("Batch latency P95"),
                QStringLiteral("%1 ms").arg(percentileMilliseconds(
                    submissionMetrics.batchLatencyNanoseconds, 0.95), 0, 'f', 3));
    printMetric(output, QStringLiteral("Batch latency P99"),
                QStringLiteral("%1 ms").arg(percentileMilliseconds(
                    submissionMetrics.batchLatencyNanoseconds, 0.99), 0, 'f', 3));
    printMetric(output, QStringLiteral("Backpressure wait"),
                QStringLiteral("%1 ms").arg(
                    submissionMetrics.backpressureWaitNanoseconds /
                    1'000'000.0, 0, 'f', 2),
                QStringLiteral("%1% of parser elapsed").arg(percent(
                    submissionMetrics.backpressureWaitNanoseconds,
                    parserNanoseconds), 0, 'f', 1));

    output << "\n[Bounded byte queue]\n";
    printMetric(output, QStringLiteral("Capacity"),
                QStringLiteral("%1 MiB").arg(parserQueue.capacity / (1024.0 * 1024.0), 0, 'f', 2));
    printMetric(output, QStringLiteral("Peak queued"),
                QStringLiteral("%1 MiB").arg(parserQueue.highWatermark / (1024.0 * 1024.0), 0, 'f', 2),
                QStringLiteral("%1% of capacity").arg(percent(parserQueue.highWatermark,
                                                               parserQueue.capacity), 0, 'f', 1));
    printMetric(output, QStringLiteral("Producer wait/fail events"),
                QString::number(parserQueue.producerWaits));
    printMetric(output, QStringLiteral("writeInput retries"),
                QString::number(parserRetries));
    printMetric(output, QStringLiteral("Bytes enqueued/dequeued"),
                QStringLiteral("%1 / %2").arg(parserQueue.totalEnqueued)
                                            .arg(parserQueue.totalDequeued));
    printMetric(output, QStringLiteral("Bytes remaining after idle"),
                QString::number(parserQueue.queuedBytes),
                parserQueue.queuedBytes == 0 ? QStringLiteral("drained")
                                             : QStringLiteral("NOT DRAINED"));

    output << "\n[Scrollback through parser]\n";
    printMetric(output, QStringLiteral("Requested lines"), QString::number(targetLines));
    printMetric(output, QStringLiteral("Stored lines"),
                QString::number(core.scrollbackLineCount()));
    printMetric(output, QStringLiteral("Elapsed"),
                QStringLiteral("%1 ms").arg(scrollbackNanoseconds / 1'000'000.0, 0, 'f', 2));
    printMetric(output, QStringLiteral("Ingestion rate"),
                QStringLiteral("%1 lines/s").arg(scrollbackLinesPerSecond, 0, 'f', 0));
    printMetric(output, QStringLiteral("writeInput retries"),
                QString::number(scrollbackRetries));
    printMetric(output, QStringLiteral("Final queue depth"),
                QStringLiteral("%1 bytes").arg(finalQueue.queuedBytes));

    output << "\n[Result]\n"
           << "  Parser correctness barrier..... "
           << (finalQueue.queuedBytes == 0 ? "PASS" : "FAIL") << '\n'
           << "  P2 throughput target........... "
           << (parserTargetMet ? "PASS" : "NOT MET") << '\n'
           << "============================================================\n";

    return 0;
}
