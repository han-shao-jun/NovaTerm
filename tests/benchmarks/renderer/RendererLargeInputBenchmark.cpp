/**
 * @file RendererLargeInputBenchmark.cpp
 * @brief Windows QRhi 大文本性能夹具：固定 64 MiB，可见/隐藏两阶段。
 * @note 参数为输出 JSON 路径、分块字节数（65536/262144）及可选
 *       --stable-timers。实际字节、队列、版本收敛与帧统计写入 JSON。
 */
#include "RendererLargeInputBenchmarkSupport.h"
#include "core/terminal/TerminalCore.h"
#include "renderer/TerminalRenderer.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>
#include <QTimer>
#include <QWindow>
#include <algorithm>
#ifndef NOMINMAX
#define NOMINMAX
#endif
// 多媒体声明依赖 Windows 基础类型，保留包含顺序。
// clang-format off
#include <windows.h>
#include <mmsystem.h>
// clang-format on

#ifndef MEASUREMENT_VARIANT
#define MEASUREMENT_VARIANT "current"
#endif

namespace
{

// 仅测量进程控制计时条件；生产代码和系统电源计划均不改变。
struct TimerResolutionScope {
    bool policyApplied{false};
    bool resolutionRequested{false};
    explicit TimerResolutionScope(bool enabled)
    {
        if (!enabled)
            return;
#ifdef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
        PROCESS_POWER_THROTTLING_STATE state{};
        state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        state.StateMask = 0;
        policyApplied = SetProcessInformation(GetCurrentProcess(),
                                              ProcessPowerThrottling,
                                              &state,
                                              static_cast<DWORD>(sizeof(state))) != FALSE;
#endif
        resolutionRequested = timeBeginPeriod(1) == TIMERR_NOERROR;
    }
    ~TimerResolutionScope()
    {
        if (resolutionRequested)
            timeEndPeriod(1);
    }
    TimerResolutionScope(const TimerResolutionScope &) = delete;
    TimerResolutionScope &operator=(const TimerResolutionScope &) = delete;
};

// 相同数据集、相同字节量，发生背压时保留未入队部分。
void waitEvents(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

double processCpuMs()
{
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        return -1;
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return double(k.QuadPart + u.QuadPart) / 10000.0;
}

QJsonObject distribution(QVector<quint64> samples)
{
    std::sort(samples.begin(), samples.end());
    QJsonObject result;
    for (const auto &p :
         {std::pair<const char *, double>{"p50_ns", .50}, {"p95_ns", .95}, {"p99_ns", .99}})
        result[p.first] =
            samples.isEmpty() ? 0.0 : double(samples[qsizetype((samples.size() - 1) * p.second)]);
    result["samples"] = samples.size();
    return result;
}

QJsonObject difference(const TerminalRenderer::RenderStatistics &a,
                       const TerminalRenderer::RenderStatistics &b)
{
    QJsonObject result;
#define METRIC(key) result[#key] = double(a.key - b.key)
    METRIC(framesRendered);
    METRIC(rowsRebuilt);
    METRIC(gpuUploadBytes);
    METRIC(contentUploadBytes);
    METRIC(atlasUploadBytes);
    METRIC(cpuFrameNanoseconds);
    METRIC(commandGenerationNanoseconds);
    METRIC(cpuFramesOverBudget);
    METRIC(glyphRasters);
    METRIC(scrollbackReflowRequests);
#undef METRIC
    result["scheduledFrames"] = double(a.scheduler.framesRequested - b.scheduler.framesRequested);
    result["memoryPeakBytes"] = double(a.memoryPeakBytes);
    return result;
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const auto options = NovaTerm::Benchmark::parseOptions(app.arguments());
    if (!options)
        return 1;
    const bool stableTimers = options->stableTimers;
    TimerResolutionScope timing(stableTimers);
    if (stableTimers && (!timing.policyApplied || !timing.resolutionRequested))
        return 6;
    const int chunkBytes = options->chunkBytes;
    constexpr qsizetype TotalBytes = 64 * 1024 * 1024;
    constexpr int LineBytes = 128;
    QByteArray corpus(TotalBytes, ' ');
    const QByteArray padding = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz";
    for (qsizetype line = 0; line < TotalBytes / LineBytes; ++line) {
        char *dst = corpus.data() + line * LineBytes;
        const QByteArray prefix =
            QByteArray("LARGE-") + QByteArray::number(line).rightJustified(10, '0') + " ";
        std::copy(prefix.begin(), prefix.end(), dst);
        for (qsizetype i = prefix.size(); i < 126; ++i)
            dst[i] = padding[i % padding.size()];
        dst[126] = '\r';
        dst[127] = '\n';
    }
    TerminalCore core(120, 40);
    core.setScrollbackLimit(10000);
    TerminalRenderer renderer(&core);
    renderer.setTargetRefreshRate(60);
    renderer.setWindowFlag(Qt::WindowStaysOnTopHint, true);
    renderer.resize(1152, 760);
    bool renderFailed = false;
    QObject::connect(&renderer, &QRhiWidget::renderFailed, [&] { renderFailed = true; });
    renderer.show();
    waitEvents(750);
    if (renderFailed || !renderer.windowHandle() || !renderer.windowHandle()->isExposed())
        return 2;
    core.writeInput("\x1b[?12h");
    core.waitForIdle();
    const auto prefill = core.writeInput(QByteArrayView(corpus.constData(), 128 * 1000));
    if (!prefill.fullyAccepted() || !core.waitForIdle(10000))
        return 3;
    waitEvents(500);
    int blinkTicks = 0;
    for (auto *t : renderer.findChildren<QTimer *>(QString(), Qt::FindDirectChildrenOnly))
        if (t->interval() == 530)
            QObject::connect(t, &QTimer::timeout, [&] { ++blinkTicks; });
    QJsonObject result;
    result["variant"] = MEASUREMENT_VARIANT;
    result["chunkBytes"] = chunkBytes;
    result["totalBytesPerScenario"] = double(TotalBytes);
    result["corpusSha256"] =
        QString::fromLatin1(QCryptographicHash::hash(corpus, QCryptographicHash::Sha256).toHex());
    result["lineBytes"] = LineBytes;
    result["linesPerScenario"] = double(TotalBytes / LineBytes);
    result["columns"] = core.columns();
    result["rows"] = core.rows();
    result["dpr"] = renderer.devicePixelRatioF();
    result["screenHz"] = renderer.windowHandle()->screen()->refreshRate();
    result["api"] = qEnvironmentVariable("NOVATERM_RHI_API");
    result["scrollbackLimit"] = 10000;
    result["producerIntervalMs"] = 5;
    result["stableTimersRequested"] = stableTimers;
    result["timerPolicyApplied"] = timing.policyApplied;
    result["timerResolutionRequested"] = timing.resolutionRequested;
    bool measurementSucceeded = true;
    for (bool visible : {true, false}) {
        if (visible)
            renderer.show();
        else
            renderer.hide();
        waitEvents(150);
        const auto mark = renderer.renderStatistics();
        const auto queueMark = core.queueStatistics();
        qsizetype sent = 0;
        int attempts = 0, partialWrites = 0, highWaterPauses = 0;
        bool timedOut = false;
        qsizetype sampledQueuePeak = 0;
        QVector<quint64> frameCpuSamples;
        quint64 priorFrames = mark.framesRendered, priorCpu = mark.cpuFrameNanoseconds;
        const auto connection = QObject::connect(&renderer, &QRhiWidget::frameSubmitted, &app, [&] {
            const auto s = renderer.renderStatistics();
            if (s.framesRendered > priorFrames) {
                frameCpuSamples.push_back(s.cpuFrameNanoseconds - priorCpu);
                priorFrames = s.framesRendered;
                priorCpu = s.cpuFrameNanoseconds;
            }
        });
        blinkTicks = 0;
        const double beforeCpu = processCpuMs();
        QElapsedTimer wall;
        wall.start();
        QVector<quint64> tickIntervals;
        tickIntervals.reserve(2048);
        qint64 priorTickNs = wall.nsecsElapsed();
        QEventLoop loop;
        QTimer producer;
        producer.setTimerType(Qt::PreciseTimer);
        producer.setInterval(5);
        QObject::connect(&producer, &QTimer::timeout, [&] {
            const auto nowNs = wall.nsecsElapsed();
            tickIntervals.push_back(quint64(nowNs - priorTickNs));
            priorTickNs = nowNs;
            const auto q = core.queueStatistics();
            sampledQueuePeak = std::max(sampledQueuePeak, q.queuedBytes);
            if (wall.elapsed() > 180000) {
                timedOut = true;
                loop.quit();
                return;
            }
            if (sent < TotalBytes) {
                if (q.queuedBytes >= 4 * 1024 * 1024) {
                    ++highWaterPauses;
                    return;
                }
                const qsizetype requested = std::min<qsizetype>(chunkBytes, TotalBytes - sent);
                const auto write =
                    core.writeInput(QByteArrayView(corpus.constData() + sent, requested));
                ++attempts;
                if (!write.fullyAccepted())
                    ++partialWrites;
                sent += write.acceptedBytes;
            } else if (core.waitForIdle(0)) {
                if (!visible ||
                    (renderer.renderProgress().lastRenderedRevision == core.modelRevision() &&
                     renderer.renderStatistics().glyphRasterQueueDepth == 0))
                    loop.quit();
            }
        });
        producer.start();
        loop.exec();
        producer.stop();
        const qint64 elapsedMs = wall.elapsed();
        const double cpuMs = processCpuMs() - beforeCpu;
        QObject::disconnect(connection);
        const auto end = renderer.renderStatistics();
        const auto queueEnd = core.queueStatistics();
        const bool parserIdle = core.waitForIdle(0);
        const bool converged = !visible || end.lastRenderedRevision == core.modelRevision();
        const bool phaseSucceeded =
            !timedOut && sent == TotalBytes &&
            queueEnd.totalDequeued - queueMark.totalDequeued == TotalBytes &&
            queueEnd.queuedBytes == 0 && parserIdle && converged;
        measurementSucceeded = measurementSucceeded && phaseSucceeded;
        auto o = difference(end, mark);
        o["acceptedBytes"] = double(sent);
        o["dequeuedBytes"] = double(queueEnd.totalDequeued - queueMark.totalDequeued);
        o["queuedBytesAtEnd"] = double(queueEnd.queuedBytes);
        o["producerAttempts"] = attempts;
        o["partialWritesRetried"] = partialWrites;
        o["highWaterPauses"] = highWaterPauses;
        o["sampledQueuePeakBytes"] = double(sampledQueuePeak);
        o["elapsedMs"] = double(elapsedMs);
        o["processCpuMs"] = cpuMs;
        o["throughputMiBPerSec"] = elapsedMs > 0 ? double(sent) / 1048576 * 1000 / elapsedMs : 0;
        o["fps"] =
            elapsedMs > 0 ? double(end.framesRendered - mark.framesRendered) * 1000 / elapsedMs : 0;
        o["frameCpuLatency"] = distribution(frameCpuSamples);
        o["producerTickIntervals"] = distribution(tickIntervals);
        o["blinkTicks"] = blinkTicks;
        o["parserIdle"] = parserIdle;
        o["converged"] = converged;
        o["timedOut"] = timedOut;
        result[visible ? "visible_output" : "hidden_output"] = o;
        waitEvents(150);
        if (!phaseSucceeded)
            break;
    }
    result["renderFailed"] = renderFailed;
    QFile output(options->outputPath);
    if (!output.open(QIODevice::WriteOnly))
        return 4;
    if (!NovaTerm::Benchmark::writeResult(output, QJsonDocument(result)))
        return 4;
    output.close();
    renderer.close();
    return renderFailed ? 5 : (measurementSucceeded ? 0 : 7);
}
