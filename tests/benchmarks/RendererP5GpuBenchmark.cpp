#include "core/terminal/TerminalCore.h"
#include "renderer/TerminalRenderer.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFont>
#include <QScreen>
#include <QTextStream>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace {

std::optional<int> integerOption(const QStringList& arguments,
                                 const QString& name,
                                 int defaultValue, int minimum, int maximum,
                                 QString& error)
{
    const int option = arguments.indexOf(name);
    if (option < 0)
        return defaultValue;
    if (option + 1 >= arguments.size()) {
        error = QStringLiteral("%1 requires an integer value").arg(name);
        return std::nullopt;
    }

    bool valid = false;
    const int value = arguments[option + 1].toInt(&valid);
    if (!valid || value < minimum || value > maximum) {
        error = QStringLiteral("%1 must be an integer in [%2, %3]")
                    .arg(name)
                    .arg(minimum)
                    .arg(maximum);
        return std::nullopt;
    }
    return value;
}

class RowSampleHistogram
{
public:
    void add(quint64 rows)
    {
        const auto bucket = size_t((std::min)(rows, OverflowBucket));
        ++_buckets[bucket];
        ++_sampleCount;
    }

    quint64 sampleCount() const { return _sampleCount; }

    quint64 percentile95() const
    {
        if (_sampleCount == 0)
            return 0;
        const quint64 target = (_sampleCount * 95 + 99) / 100;
        quint64 cumulative = 0;
        for (size_t bucket = 0; bucket < _buckets.size(); ++bucket) {
            cumulative += _buckets[bucket];
            if (cumulative >= target)
                return quint64(bucket);
        }
        return OverflowBucket;
    }

private:
    static constexpr quint64 OverflowBucket = 256;
    std::array<quint64, size_t(OverflowBucket + 1)> _buckets{};
    quint64 _sampleCount{0};
};

void waitEvents(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

template<typename Predicate>
bool waitUntil(Predicate predicate, int timeoutMilliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMilliseconds)
        waitEvents(10);
    return predicate();
}

template<typename Predicate>
bool waitForStage(Predicate predicate, int timeoutMilliseconds,
                  QStringView stage)
{
    if (waitUntil(std::move(predicate), timeoutMilliseconds))
        return true;
    QTextStream(stderr) << "P5 QRhi benchmark " << stage
                        << " timed out\n";
    return false;
}

bool writeAndDrain(TerminalCore& core, QByteArrayView input,
                   int timeoutMilliseconds, QStringView stage)
{
    const auto result = core.writeInput(input);
    if (!result.fullyAccepted()) {
        QTextStream(stderr)
            << "P5 QRhi benchmark " << stage << " input backpressured: "
            << result.acceptedBytes << '/' << result.requestedBytes << '\n';
        return false;
    }
    if (!core.waitForIdle(timeoutMilliseconds)) {
        QTextStream(stderr) << "P5 QRhi benchmark " << stage
                            << " parser timed out\n";
        return false;
    }
    return true;
}

TerminalRenderer::RenderStatistics delta(
    const TerminalRenderer::RenderStatistics& a,
    const TerminalRenderer::RenderStatistics& b)
{
    auto r = a;
#define DELTA(field) r.field -= b.field
    DELTA(rowsRebuilt); DELTA(commandsGenerated);
    DELTA(commandGenerationNanoseconds); DELTA(cpuFrameNanoseconds);
    DELTA(gpuUploadBytes); DELTA(contentUploadBytes); DELTA(atlasUploadBytes);
    DELTA(drawCalls); DELTA(vertexBufferReallocations);
    DELTA(revisionPromotedFullFrames); DELTA(revisionRecoveredRows);
    DELTA(framesRendered); DELTA(cpuFramesOverBudget);
    DELTA(dirtyBlocksRebuilt); DELTA(mappingOnlyUpdates);
    DELTA(rowSlotsReused); DELTA(rowSlotsCreated);
    DELTA(glyphCacheHits); DELTA(glyphCacheMisses); DELTA(glyphRasters);
    DELTA(glyphEvictions); DELTA(capabilityFallbacks);
    DELTA(scrollbackReflowRequests);
#undef DELTA
    return r;
}

void print(QTextStream& out, const QString& name,
           const TerminalRenderer::RenderStatistics& s)
{
    out << name << ": frames=" << s.framesRendered
        << ", rows=" << s.rowsRebuilt
        << ", blocks=" << s.dirtyBlocksRebuilt
        << ", content_upload=" << s.contentUploadBytes
        << ", atlas_upload=" << s.atlasUploadBytes
        << ", total_upload=" << s.gpuUploadBytes
        << ", draws=" << s.drawCalls
        << ", instances=" << s.commandsGenerated
        << ", cache_hit=" << s.glyphCacheHits
        << ", cache_miss=" << s.glyphCacheMisses
        << ", raster=" << s.glyphRasters
        << ", eviction=" << s.glyphEvictions
        << ", slot_reused=" << s.rowSlotsReused
        << ", slot_new=" << s.rowSlotsCreated
        << ", buffer_realloc=" << s.vertexBufferReallocations
        << ", cpu_over_budget=" << s.cpuFramesOverBudget
        << ", reflow_requests=" << s.scrollbackReflowRequests
        << ", revision_recovered=" << s.revisionRecoveredRows << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    const QStringList args = app.arguments();
    QString optionError;
    const auto durationOption = integerOption(
        args, QStringLiteral("--duration-ms"), 5000, 1000,
        24 * 60 * 60 * 1000, optionError);
    const auto refreshOption = integerOption(
        args, QStringLiteral("--refresh-rate"), 60, 60, 144, optionError);
    const auto scrollbackOption = integerOption(
        args, QStringLiteral("--scrollback-limit"), 100'000, 0, 1'000'000,
        optionError);
    const auto prefillOption = integerOption(
        args, QStringLiteral("--prefill-lines"), 0, 0, 1'000'000,
        optionError);
    if (!durationOption || !refreshOption || !scrollbackOption
        || !prefillOption) {
        QTextStream(stderr) << "P5 QRhi benchmark: " << optionError << '\n';
        return 1;
    }
    const int durationMs = *durationOption;
    const int refreshRate = *refreshOption;
    const int scrollbackLimit = *scrollbackOption;
    const int prefillLines = *prefillOption;
    if (refreshRate != 60 && refreshRate != 120 && refreshRate != 144) {
        QTextStream(stderr)
            << "P5 QRhi benchmark: --refresh-rate must be 60, 120, or 144\n";
        return 1;
    }

    TerminalCore core(119, 40);
    core.setScrollbackLimit(scrollbackLimit);
    if (!core.waitForIdle()) {
        QTextStream(stderr)
            << "P5 QRhi benchmark scrollback-limit setup timed out\n";
        return 4;
    }
    TerminalRenderer renderer(&core);
    // This benchmark measures the opt-in row-slot reuse path. Production
    // native Windows shells use conservative live-scroll rendering.
    renderer.setConservativeLiveScrollRendering(false);
    const bool allowOcclusion = args.contains(
        QStringLiteral("--allow-occlusion"));
    if (!allowOcclusion)
        renderer.setWindowFlag(Qt::WindowStaysOnTopHint, true);
    bool failed = false;
    QObject::connect(&renderer, &QRhiWidget::renderFailed,
                     [&failed]() { failed = true; });
    renderer.resize(1152, 760);
    renderer.setTargetRefreshRate(refreshRate);
    renderer.show();
    renderer.raise();
    renderer.activateWindow();
    waitEvents(750);
    if (failed || !renderer.windowHandle()
        || !renderer.windowHandle()->isExposed()) {
        QTextStream(stderr) << "P5 QRhi benchmark could not expose widget\n";
        return 2;
    }

    constexpr int PrefillBatchLines = 10'000;
    for (int first = 0; first < prefillLines; first += PrefillBatchLines) {
        const int count = std::min(PrefillBatchLines, prefillLines - first);
        QByteArray input;
        input.reserve(count * 16);
        for (int line = 0; line < count; ++line) {
            input += QByteArrayLiteral("P5-prefill-");
            input += QByteArray::number(first + line);
            input += QByteArrayLiteral("\r\n");
        }
        if (!writeAndDrain(core, input, 30'000,
                           QStringView(u"prefill"))) {
            return 4;
        }
        waitEvents(25);
    }
    if (prefillLines > 0)
        waitEvents(250);

    auto mark = renderer.renderStatistics();
    if (!writeAndDrain(core, QByteArrayLiteral("\x1b[1;1HX"), 5000,
                       QStringView(u"single-cell"))) {
        return 4;
    }
    waitEvents(100);
    auto now = renderer.renderStatistics();
    const auto single = delta(now, mark); mark = now;

    if (!writeAndDrain(core,
                       QByteArrayLiteral("\x1b[2;1HAAAAAAAAAAAAAAAA"),
                       5000, QStringView(u"ASCII cache prime"))) {
        return 4;
    }
    waitEvents(100);
    mark = renderer.renderStatistics();
    if (!writeAndDrain(core,
                       QByteArrayLiteral("\x1b[3;1HAAAAAAAAAAAAAAAA"),
                       5000, QStringView(u"ASCII warm cache"))) {
        return 4;
    }
    waitEvents(100);
    now = renderer.renderStatistics();
    const auto warm = delta(now, mark); mark = now;

    const QByteArray complexCorpus = QStringLiteral(
        "中文 e\u0301 한글 日本語 ┌─┐ \ue0b0 👩‍💻 🧑🏽‍🚀").toUtf8();
    if (!writeAndDrain(core,
                       QByteArrayLiteral("\x1b[4;1H") + complexCorpus,
                       5000, QStringView(u"complex cache prime"))) {
        return 4;
    }
    waitEvents(150);
    now = renderer.renderStatistics();
    const auto complex = delta(now, mark); mark = now;

    if (!writeAndDrain(core,
                       QByteArrayLiteral("\x1b[5;1H") + complexCorpus,
                       5000, QStringView(u"complex warm cache"))) {
        return 4;
    }
    waitEvents(150);
    now = renderer.renderStatistics();
    const auto complexWarm = delta(now, mark); mark = now;

    if (!writeAndDrain(core, QByteArrayLiteral("\x1b[6;5H"), 5000,
                       QStringView(u"overlay"))) {
        return 4;
    }
    waitEvents(100);
    now = renderer.renderStatistics();
    const auto overlay = delta(now, mark); mark = now;

    quint64 line = 0;
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    const int producerIntervalMs = std::max(1, (1000 + refreshRate - 1)
                                               / refreshRate);
    timer.setInterval(producerIntervalMs);
    bool producerAccepted = true;
    qsizetype producerRejectedBytes = 0;
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        const QByteArray input = QByteArrayLiteral("\r\nP5-scroll-")
            + QByteArray::number(++line);
        const auto result = core.writeInput(input);
        if (!result.fullyAccepted()) {
            producerAccepted = false;
            producerRejectedBytes += result.requestedBytes
                - result.acceptedBytes;
        }
    });
    RowSampleHistogram rowSamples;
    quint64 sampledRows = renderer.renderProgress().rowsRebuilt;
    QTimer sampler;
    sampler.setTimerType(Qt::PreciseTimer);
    sampler.setInterval(producerIntervalMs);
    QObject::connect(&sampler, &QTimer::timeout, [&]() {
        const quint64 current = renderer.renderProgress().rowsRebuilt;
        rowSamples.add(current - sampledRows);
        sampledRows = current;
    });
    QElapsedTimer elapsed;
    elapsed.start(); timer.start(); sampler.start();
    waitEvents(durationMs); timer.stop(); sampler.stop();
    const qint64 scrollMs = elapsed.elapsed();
    const quint64 scrollInputTicks = line;
    if (!producerAccepted) {
        QTextStream(stderr)
            << "P5 QRhi benchmark steady-scroll input backpressured: "
            << producerRejectedBytes << " bytes rejected\n";
        return 4;
    }
    if (!core.waitForIdle(10'000)) {
        QTextStream(stderr)
            << "P5 QRhi benchmark steady-scroll parser timed out\n";
        return 4;
    }
    if (!waitForStage([&]() {
            return renderer.renderProgress().lastRenderedRevision
                == core.modelRevision();
        }, 5000, QStringView(u"steady-scroll convergence"))) {
        return 4;
    }
    now = renderer.renderStatistics();
    const auto scroll = delta(now, mark); mark = now;

    quint64 expectedFrame = now.framesRendered + 1;
    renderer.setColorScheme(renderer.colorScheme());
    if (!waitForStage([&]() {
            const auto progress = renderer.renderProgress();
            return progress.framesRendered >= expectedFrame
                && progress.lastRenderedRevision == core.modelRevision();
        }, 5000, QStringView(u"forced-full frame"))) {
        return 4;
    }
    now = renderer.renderStatistics();
    const auto full = delta(now, mark); mark = now;

    expectedFrame = now.framesRendered + 1;
    const int columnsBeforeResize = core.columns();
    const int rowsBeforeResize = core.rows();
    // Exercise the production failure order: output scrolls immediately
    // before the async grid resize and continues while the resize full-frame
    // request crosses the GUI/render scheduler boundary.
    const auto resizeInput = core.writeInput(
        QByteArrayLiteral("\r\nP5-resize-scroll"));
    if (!resizeInput.fullyAccepted()) {
        QTextStream(stderr)
            << "P5 QRhi benchmark resize input backpressured\n";
        return 4;
    }
    producerAccepted = true;
    producerRejectedBytes = 0;
    timer.start();
    renderer.resize(1060, 700);
    if (!waitForStage([&]() {
            if (core.columns() == columnsBeforeResize
                && core.rows() == rowsBeforeResize) {
                return false;
            }
            return renderer.renderProgress().framesRendered >= expectedFrame;
        }, 5000, QStringView(u"resize frame"))) {
        timer.stop();
        return 4;
    }
    waitEvents(250);
    timer.stop();
    if (!producerAccepted) {
        QTextStream(stderr)
            << "P5 QRhi benchmark resize input backpressured: "
            << producerRejectedBytes << " bytes rejected\n";
        return 4;
    }
    if (!core.waitForIdle(10'000)) {
        QTextStream(stderr) << "P5 QRhi benchmark resize parser timed out\n";
        return 4;
    }
    if (!waitForStage([&]() {
            return renderer.renderProgress().lastRenderedRevision
                == core.modelRevision();
        }, 5000, QStringView(u"resize convergence"))) {
        return 4;
    }
    const int rowsAfterResize = core.rows();
    now = renderer.renderStatistics();
    const auto resize = delta(now, mark); mark = now;

    // Keep both grayscale and color pages visible while exercising resource
    // invalidation so a deferred second-page upload must complete by itself.
    if (!writeAndDrain(core,
                       QByteArrayLiteral("\x1b[1;1H") + complexCorpus,
                       5000, QStringView(u"font rebuild prime"))) {
        return 4;
    }
    waitEvents(150);
    mark = renderer.renderStatistics();
    QFont changed = renderer.font();
    changed.setPixelSize(changed.pixelSize() + 1);
    renderer.setFont(changed);
    if (!core.waitForIdle(10'000)) {
        QTextStream(stderr)
            << "P5 QRhi benchmark font resize parser timed out\n";
        return 4;
    }
    expectedFrame = mark.framesRendered + 1;
    if (!waitForStage([&]() {
            const auto progress = renderer.renderProgress();
            return progress.framesRendered >= expectedFrame
                && progress.lastRenderedRevision == core.modelRevision();
        }, 5000, QStringView(u"font rebuild"))) {
        return 4;
    }
    now = renderer.renderStatistics();
    const auto font = delta(now, mark);

    const auto final = renderer.renderStatistics();
    const quint64 rowsP95 = rowSamples.percentile95();
    QTextStream out(stdout);
    const QScreen* screen = renderer.windowHandle()->screen();
    out << "NovaTerm P5 QRhi benchmark\napi="
        << qEnvironmentVariable("NOVATERM_RHI_API", "default")
        << ", grid=" << core.columns() << 'x' << core.rows()
        << ", duration_ms=" << scrollMs
        << ", target_refresh_hz=" << refreshRate
        << ", screen_refresh_hz=" << (screen ? screen->refreshRate() : 0.0)
        << ", dpr=" << renderer.devicePixelRatioF()
        << ", qt_scale_factor="
        << qEnvironmentVariable("QT_SCALE_FACTOR", "default")
        << ", scrollback_limit=" << scrollbackLimit
        << ", prefill_lines=" << prefillLines
        << ", stays_on_top=" << (allowOcclusion ? "false" : "true")
        << '\n';
    print(out, QStringLiteral("single_cell"), single);
    print(out, QStringLiteral("warm_cache"), warm);
    print(out, QStringLiteral("cjk_combining"), complex);
    print(out, QStringLiteral("complex_warm_cache"), complexWarm);
    print(out, QStringLiteral("overlay_only"), overlay);
    print(out, QStringLiteral("steady_scroll"), scroll);
    print(out, QStringLiteral("forced_full"), full);
    print(out, QStringLiteral("resize"), resize);
    print(out, QStringLiteral("font_dpi_rebuild"), font);
    out << "scroll_fps=" << (scrollMs ? scroll.framesRendered * 1000.0
                                         / scrollMs : 0.0)
        << ", scroll_samples=" << rowSamples.sampleCount()
        << ", scroll_input_ticks=" << scrollInputTicks
        << ", scroll_rows_p95=" << rowsP95
        << ", cpu_ns[p50/p95/p99]=" << final.cpuFrameP50Nanoseconds << '/'
        << final.cpuFrameP95Nanoseconds << '/'
        << final.cpuFrameP99Nanoseconds
        << ", buffer_current=" << final.bufferCurrentBytes
        << ", buffer_peak=" << final.bufferPeakBytes
        << ", atlas_current=" << final.atlasCurrentBytes
        << ", atlas_peak=" << final.atlasPeakBytes
        << ", memory_current=" << final.memoryCurrentBytes
        << ", memory_peak=" << final.memoryPeakBytes
        << ", raster_queue_depth=" << final.glyphRasterQueueDepth
        << ", raster_queue_peak=" << final.glyphRasterQueuePeakDepth
        << ", raster_queue_rejected=" << final.glyphRasterQueueRejected
        << ", raster_queue_cancelled=" << final.glyphRasterQueueCancelled
        << ", raster_queue_stale=" << final.glyphRasterQueueStaleDropped
        << ", scrollback_lines=" << core.scrollbackLineCount()
        << ", scrollback_evicted="
        << core.scrollbackStatistics().evictedLines
        << ", published_revision=" << core.modelRevision()
        << ", rendered_revision=" << final.lastRenderedRevision
        << ", converged="
        << (core.modelRevision() == final.lastRenderedRevision ? "true" : "false")
        << '\n';

    QStringList acceptanceFailures;
    const auto require = [&acceptanceFailures](bool condition,
                                                const QString& failure) {
        if (!condition)
            acceptanceFailures.push_back(failure);
    };
    require(warm.glyphRasters == 0 && warm.atlasUploadBytes == 0,
            QStringLiteral("warm ASCII cache was not stable"));
    require(complexWarm.glyphRasters == 0
                && complexWarm.atlasUploadBytes == 0,
            QStringLiteral("warm complex cache was not stable"));
    require(overlay.rowsRebuilt == 0 && overlay.contentUploadBytes == 0
                && overlay.atlasUploadBytes == 0,
            QStringLiteral("overlay-only frame changed base content"));
    constexpr quint64 MinimumScrollSamples = 10;
    require(rowSamples.sampleCount() >= MinimumScrollSamples
                && scrollInputTicks >= MinimumScrollSamples,
            QStringLiteral("steady-scroll workload produced too few samples"));
    require(rowsP95 <= 2,
            QStringLiteral("steady-scroll row P95 exceeded 2"));
    require(scroll.scrollbackReflowRequests == 0,
            QStringLiteral("steady-scroll requested history reflow"));
    require(scroll.vertexBufferReallocations == 0,
            QStringLiteral("steady-scroll buffer reallocated"));
    require(scroll.cpuFramesOverBudget == 0,
            QStringLiteral("steady-scroll frame exceeded CPU budget"));
    require(full.rowsRebuilt > 0,
            QStringLiteral("forced-full scenario did not rebuild content"));
    require(resize.rowsRebuilt > 0,
            QStringLiteral("resize scenario did not rebuild content"));
    constexpr quint64 GpuInstanceBytes = sizeof(float) * 16;
    const quint64 retainedStrideUploadBytes =
        quint64(rowsAfterResize) * quint64(columnsBeforeResize) * 5
        * GpuInstanceBytes;
    require(resize.contentUploadBytes >= retainedStrideUploadBytes,
            QStringLiteral("resize did not clear retained row strides"));
    require(font.rowsRebuilt > 0 && font.atlasUploadBytes > 0,
            QStringLiteral("font/DPI scenario did not restore content"));
    require(final.glyphRasterQueueDepth == 0
                && final.glyphRasterQueueRejected == 0,
            QStringLiteral("raster queue did not drain cleanly"));
    require(core.modelRevision() == final.lastRenderedRevision,
            QStringLiteral("final revision did not converge"));
    out << "acceptance="
        << (acceptanceFailures.isEmpty() ? "pass" : "fail");
    if (!acceptanceFailures.isEmpty())
        out << ", reasons=" << acceptanceFailures.join(QStringLiteral("; "));
    out << '\n';
    renderer.close();
    if (failed)
        return 3;
    return acceptanceFailures.isEmpty() ? 0 : 5;
}
