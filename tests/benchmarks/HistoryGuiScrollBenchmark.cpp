/** Real QRhi history-scroll probe. GPU dependent; never a default CTest. */
#include "core/terminal/TerminalCore.h"
#include "renderer/TerminalRenderer.h"
#include "history/HistoryBenchmarkSupport.h"
#include "HistoryGuiScrollBenchmarkSupport.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextStream>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
qint64 nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void events(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}
template<class Predicate>
void await(Predicate predicate, int timeout = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        require(timer.elapsed() < timeout, "GUI stage timed out");
        events(1);
    }
}
QJsonObject distribution(std::vector<double> values)
{
    QJsonObject result{{"samples", int(values.size())}};
    if (values.empty()) return result;
    std::sort(values.begin(), values.end());
    for (const auto& p : {std::pair<const char*, double>{"p50_ms", .5}, {"p95_ms", .95}, {"p99_ms", .99}}) {
        result[p.first] = values[std::max<std::size_t>(1, std::size_t(std::ceil(p.second * double(values.size())))) - 1];
    }
    result["min_ms"] = values.front(); result["max_ms"] = values.back();
    return result;
}
QColor marker(NovaTerm::LineId id)
{
    const auto bits = (id * 0x9e3779ULL) & 0xffffffULL;
    return QColor(int((bits >> 16) & 255), int((bits >> 8) & 255), int(bits & 255));
}
QByteArray logLine(quint64 row, bool mixed)
{
    const auto color = marker(row + 1);
    QByteArray line = "\x1b[48;2;" + QByteArray::number(color.red()) + ';'
        + QByteArray::number(color.green()) + ';' + QByteArray::number(color.blue()) + "m \x1b[0mROW-"
        + QByteArray::number(row).rightJustified(7, '0') + " sensor received temperature=25.75 OK ";
    line += mixed ? QStringLiteral("中🙂色 日志 ").toUtf8() : QByteArray("ascii log ");
    line += QByteArray(12, char('a' + row % 26));
    line += "\r\n";
    return line;
}
void selfTest()
{
    TerminalCore core(120, 40);
    auto runtime = std::make_shared<NovaTerm::HistoryCompressionService>();
    NovaTerm::HistoryCompressionPolicy policy;
    policy.enabled = true; policy.hotBytes = 0;
    require(core.configureHistory(runtime, policy) && core.waitForIdle(), "history setup failed");
    TerminalRenderer renderer(&core);
    renderer.resize(1152, 760);
    bool failed = false;
    QObject::connect(&renderer, &QRhiWidget::renderFailed, &renderer, [&] { failed = true; });
    renderer.show();
    await([&] { return failed || (renderer.windowHandle() && renderer.windowHandle()->isExposed()); });
    require(!failed, "QRhi initialization failed");
    require(core.writeInput(QByteArray("history-row\r\n").repeated(3000)).fullyAccepted(), "input rejected");
    require(core.waitForIdle(), "parser timeout");
    await([&] { return renderer.maximumScrollOffset() > 2000 && runtime->statistics().outstandingTasks == 0; });
    await([&] { return renderer.historyDisplayRowCount() == core.scrollbackLineCount(); });
    await([&] { return renderer.renderStatistics().glyphRasterQueueDepth == 0 && renderer.renderProgress().framesRendered > 0; });
    events(80); // settle prefill handoffs before observing this scroll's preparation
    const auto history = core.scrollbackSnapshot();
    auto reader = history.chunks().front().chunk->reader;
    require(bool(reader), "history reader missing");
    require(core.writeInput("active-tail\r\n").fullyAccepted() && core.waitForIdle(), "active tail setup failed");
    await([&] { return renderer.historyDisplayRowCount() == core.scrollbackLineCount(); });
    events(80);
    const auto before = renderer.renderStatistics();
    auto pendingCount = before.historyPendingFrames;
    bool pendingPreserved = true, sawPending = false;
    const auto observation = QObject::connect(&renderer, &QRhiWidget::frameSubmitted, &renderer, [&] {
        const auto state = renderer.renderStatistics();
        if (state.historyPendingFrames > pendingCount) {
            sawPending = true;
            pendingPreserved = pendingPreserved && state.completedViewportRevision == before.completedViewportRevision;
            pendingCount = state.historyPendingFrames;
        }
    });
    // Hold cold delivery so early viewport preparation cannot make the Pending
    // preservation check disappear on a fast worker/GPU.
    std::vector<std::shared_ptr<const NovaTerm::EncodedHistoryBlock>> held;
    auto* preparationScheduler = renderer.findChild<NovaTerm::RenderScheduler*>();
    int handoffs = 0;
    int handoffsAtFirstRequest = -1;
    reader->setColdRequest([&held, &handoffs, &handoffsAtFirstRequest](auto block) {
        if (held.empty()) handoffsAtFirstRequest = handoffs;
        held.push_back(std::move(block));
        return true;
    });
    const auto preparationCheck = QObject::connect(preparationScheduler, &NovaTerm::RenderScheduler::frameRequested,
        &renderer, [&] { ++handoffs; });
    const auto activeBefore = core.scrollbackStatistics();
    require(activeBefore.activeLines > 0, "active tail missing");
    renderer.scrollToLine(2000);
    // Run only queued preparation, before the normal GPU frame can publish a
    // render snapshot. Preflight must leave the parser's active tail intact.
    QCoreApplication::sendPostedEvents(&renderer, QEvent::MetaCall);
    require(core.scrollbackStatistics().activeLines == activeBefore.activeLines,
            "cold preparation must not seal active history");
    await([&] { return !held.empty(); });
    require(handoffsAtFirstRequest == 0, "scroll must prepare cold data before the first GPU frame handoff");
    QObject::disconnect(preparationCheck);
    await([&] { return sawPending; });
    for (const auto& block : held) {
        const auto loaded = reader->read(block, 0, NovaTerm::HistoryReadMode::Synchronous);
        require(bool(loaded), "held cold decode failed");
        reader->completeCold(block, loaded);
    }
    reader->notify();
    await([&] {
        const auto progress = renderer.renderProgress();
        return progress.framesRendered > before.framesRendered
            && progress.rowsRebuilt >= before.rowsRebuilt + quint64(core.rows())
            && renderer.renderStatistics().glyphRasterQueueDepth == 0
            && renderer.renderStatistics().completedScrollOffset == 2000;
    });
    const auto completed = renderer.renderStatistics();
    require(completed.completedScrollOffset == 2000, "completed viewport must identify the actual history target");
    require(completed.completedHistoryAnchor != 0, "completed history viewport must retain its line anchor");
    require(completed.completedViewportRevision > before.completedViewportRevision, "completed viewport revision did not advance");
    require(sawPending && pendingPreserved, "pending history must preserve the previous completed viewport");
    reader->setColdRequest({});
    QObject::disconnect(observation);
    events(80);
    auto* scheduler = renderer.findChild<NovaTerm::RenderScheduler*>();
    require(scheduler != nullptr, "render scheduler missing");
    scheduler->cancel();
    const auto beforeInterleaving = renderer.renderStatistics();
    // Deliver an already queued sparse frame, then change the viewport before
    // the throttled full-frame request is handed off. Readback renders now,
    // without waiting for the scheduler timer, so this interleaving is exact.
    scheduler->frameRequested({{0, 1, 0, 1}}, false, false, core.modelRevision());
    renderer.scrollToLine(1999);
    require(!renderer.grabFramebuffer().isNull(), "interleaving readback failed");
    require(renderer.renderStatistics().completedViewportRevision == beforeInterleaving.completedViewportRevision,
            "sparse frame must not complete a new viewport mapping");
    await([&] { return renderer.renderStatistics().completedScrollOffset == 1999; });
    // Evict the displayed anchor before its layout update is delivered. Check
    // both an explicit full draw and a sparse draw promoted by changed content.
    for (bool full : {true, false}) {
        scheduler->cancel();
        const auto beforeEvictionDraw = renderer.renderStatistics();
        core.setScrollbackLimit(core.rows() * (full ? 3 : 2));
        require(core.waitForIdle(), "eviction setup failed");
        const auto retained = core.scrollbackSnapshot();
        require(retained.firstLineId() > beforeEvictionDraw.completedHistoryAnchor, "anchor was not evicted");
        // Isolate anchor provenance from the separately tested Pending path.
        for (int row = 0; row < core.rows(); ++row)
            require(bool(retained.lineAt(row, NovaTerm::HistoryReadMode::Synchronous)), "fallback preload failed");
        scheduler->frameRequested({{0, 1, 0, 1}}, full, false, core.modelRevision());
        require(!renderer.grabFramebuffer().isNull(), "eviction readback failed");
        require(renderer.renderStatistics().rowsRebuilt >= beforeEvictionDraw.rowsRebuilt + quint64(core.rows()),
                "fallback must rebuild the entire viewport before completion");
        require(renderer.renderStatistics().completedHistoryAnchor == retained.firstLineId(),
                "completed anchor must identify the drawn fallback after eviction");
    }
    renderer.close();
}

enum class ScrollGesture { Wheel, Page, Bar };
struct RequestEvent final : QEvent {
    static QEvent::Type typeId() { static const auto type = QEvent::Type(QEvent::registerEventType()); return type; }
    int id;
    ScrollGesture gesture;
    int value;
    qint64 posted;
    RequestEvent(int number, ScrollGesture kind, int position)
        : QEvent(typeId()), id(number), gesture(kind), value(position), posted(nowNs()) {}
};
class ProbeRenderer final : public TerminalRenderer {
public:
    ProbeRenderer() = delete; // MSVC /Wall: the inherited base constructor requires a core.
    using TerminalRenderer::TerminalRenderer;
    std::function<void(const RequestEvent&)> dispatch;
    std::function<void(const char*)> trace;
protected:
    void render(QRhiCommandBuffer* cb) override
    {
        if (trace) trace("render_begin");
        TerminalRenderer::render(cb);
        if (trace) trace("render_end");
    }
    bool event(QEvent* event) override
    {
        if (event->type() == RequestEvent::typeId()) {
            if (dispatch) dispatch(*static_cast<RequestEvent*>(event));
            return true;
        }
        return TerminalRenderer::event(event);
    }
};
struct Producer {
    std::atomic<bool> stop{false};
    std::thread worker;
    ~Producer() { stop.store(true); if (worker.joinable()) worker.join(); }
    Producer() = default;
    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;
};
struct Request {
    qint64 posted{0}, handled{0}, completed{0};
    NovaTerm::LineId anchor{0};
    quint64 beforeRevision{0};
    int offset{0};
    qint64 supersededAt{0};
    bool coalesced{false};
};
QString option(const QStringList& args, const QString& key, const QString& fallback)
{
    const auto index = args.indexOf(key);
    if (index < 0) return fallback;
    require(index + 1 < args.size(), "option needs a value");
    return args[index + 1];
}
int integerOption(const QStringList& args, const QString& key, int fallback, int minimum, int maximum)
{
    bool valid = false;
    const int value = option(args, key, QString::number(fallback)).toInt(&valid);
    require(valid && value >= minimum && value <= maximum, "integer option out of range");
    return value;
}
QJsonObject measure(QApplication& app)
{
    const auto args = app.arguments();
    const auto profile = option(args, "--profile", "on-default");
    const auto corpus = option(args, "--corpus", "variable");
    const bool tracePipeline = args.contains("--trace-pipeline");
    require(profile == "off" || profile == "on-default" || profile == "on-small" || profile == "on-hot", "unknown profile");
    require(corpus == "variable" || corpus == "mixed", "unknown corpus");
    const int count = integerOption(args, "--samples", 128, 16, 1000);
    const int rows = integerOption(args, "--rows", 50000, 10000, 200000);
    const bool mixed = corpus == "mixed";
    auto runtime = std::make_shared<NovaTerm::HistoryCompressionService>();
    TerminalCore core(120, 40);
    NovaTerm::HistoryCompressionPolicy policy;
    policy.enabled = profile != "off";
    if (profile == "on-small") policy.targetBlockBytes = 256 * 1024;
    if (profile == "on-hot") policy.hotBytes = 8 * 1024 * 1024;
    require(policy.enabled || (!args.contains("--block-kib") && !args.contains("--hot-mib")),
            "compression parameters require an ON profile");
    policy.targetBlockBytes = std::size_t(integerOption(args, "--block-kib",
        int(policy.targetBlockBytes / 1024), 4, 1024)) * 1024;
    policy.hotBytes = std::size_t(integerOption(args, "--hot-mib",
        int(policy.hotBytes / (1024 * 1024)), 0, 8)) * 1024 * 1024;
    require(core.configureHistory(runtime, policy) && core.waitForIdle(), "history configuration failed");
    require(!policy.enabled || core.historyCompressionAvailable(), "compression admission failed");
    QWidget host;
    host.setWindowTitle(QStringLiteral("NovaTerm history GUI benchmark — ") + profile + " / " + corpus);
    auto* layout = new QHBoxLayout(&host);
    layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    ProbeRenderer renderer(&core);
    QScrollBar bar(Qt::Vertical);
    layout->addWidget(&renderer, 1); layout->addWidget(&bar);
    QObject::connect(&renderer, &TerminalRenderer::scrollStateChanged, &bar, [&](int maximum, int offset) {
        const QSignalBlocker blocked(&bar);
        bar.setRange(0, maximum); bar.setPageStep(core.rows()); bar.setValue(maximum - offset);
    });
    QObject::connect(&bar, &QScrollBar::valueChanged, &renderer, [&](int value) { renderer.scrollToLine(bar.maximum() - value); });
    bool failed = false;
    QObject::connect(&renderer, &QRhiWidget::renderFailed, &host, [&] { failed = true; });
    host.resize(1200, 800);
    host.setWindowFlag(Qt::WindowStaysOnTopHint, true);
    renderer.setTargetRefreshRate(60);
    host.show(); host.raise();
    await([&] { return failed || (host.windowHandle() && host.windowHandle()->isExposed()); });
    require(!failed, "GPU window unavailable");
    events(250);
    require(core.waitForIdle() && core.columns() >= 95, "benchmark viewport too narrow");
    require(core.writeInput("\x1b[?25l").fullyAccepted(), "cursor setup rejected");
    const auto prefillBegin = nowNs();
    const auto prefillCpuBegin = HistoryBenchmark::processCpuMs();
    for (int first = 0; first < rows; first += 512) {
        QByteArray bytes;
        for (int row = first; row < (std::min)(rows, first + 512); ++row) bytes += logLine(quint64(row), mixed);
        require(core.writeInput(bytes).fullyAccepted() && core.waitForIdle(10000), "prefill rejected/timed out");
        events(1);
    }
    await([&] { return runtime->statistics().outstandingTasks == 0 && renderer.historyDisplayRowCount() == core.scrollbackLineCount(); });
    QTextStream(stderr) << "prefill settled: completed=" << renderer.renderStatistics().completedScrollOffset
        << " mapping=" << renderer.renderStatistics().viewportMappingRevision
        << " completed revision=" << renderer.renderStatistics().completedViewportRevision << '\n';
    await([&] { return renderer.renderStatistics().completedScrollOffset == 0 && renderer.renderStatistics().glyphRasterQueueDepth == 0; });
    const auto prefillMs = double(nowNs() - prefillBegin) / 1e6;
    const auto prefillCpuMs = HistoryBenchmark::processCpuMs() - prefillCpuBegin;
    const auto initial = core.scrollbackStatistics();
    require(initial.evictedLines == 0 && initial.logicalLines > rows - 100, "prefill content/retention differs");
    require(!policy.enabled || initial.compressedBlocks > 0, "ON has no compressed blocks");
    const auto initialMemory = runtime->statistics();
    const int maximum = renderer.maximumScrollOffset();
    require(maximum > (count + 70) * core.rows(), "too little history for page-scroll sample count");
    const auto geometry = core.screenSize();
    QJsonArray scenarios;
    std::vector<double> pixelReadMs;
    int pixelChecks = 0;
    auto checkPixels = [&](NovaTerm::LineId anchor) {
        const auto begin = nowNs();
        const auto image = renderer.grabFramebuffer();
        pixelReadMs.push_back(double(nowNs() - begin) / 1e6);
        require(!image.isNull(), "framebuffer readback failed");
        const auto pixel = image.pixelColor((std::min)(2, image.width() - 1), (std::min)(2, image.height() - 1));
        const auto expected = marker(anchor);
        require(std::abs(pixel.red() - expected.red()) <= 3 && std::abs(pixel.green() - expected.green()) <= 3
            && std::abs(pixel.blue() - expected.blue()) <= 3, "target history pixel marker mismatch");
        ++pixelChecks;
    };
    quint64 nextRow = quint64(rows);
    for (int stage = 0; stage < 4; ++stage) {
        const QString name = QStringList{"hot_wheel", "cold_pages", "cold_scrollbar_jumps", "live_input_wheel"}[stage];
        QTextStream(stderr) << "starting " << name << '\n';
        renderer.scrollToLine(stage == 0 ? 30 : stage == 3 ? maximum / 2 : maximum - 2000);
        await([&] { return renderer.renderStatistics().completedScrollOffset == renderer.scrollOffset(); });
        events(100);
        const auto before = renderer.renderStatistics();
        const auto memoryBefore = runtime->statistics();
        const auto cpuBefore = HistoryBenchmark::processCpuMs();
        const auto stageBegin = nowNs();
        QJsonArray pipeline;
        auto trace = [&](const char* kind) {
            if (!tracePipeline) return;
            require(pipeline.size() < 16384, "pipeline trace capacity exceeded");
            const auto state = renderer.renderStatistics();
            pipeline.append(QJsonObject{{"kind", kind}, {"at_ms", double(nowNs() - stageBegin) / 1e6},
                {"offset", renderer.scrollOffset()}, {"completed_offset", state.completedScrollOffset},
                {"pending_frames", double(state.historyPendingFrames)},
                {"completed_revision", double(state.completedViewportRevision)}});
        };
        if (tracePipeline) renderer.trace = trace;
        auto* scheduler = renderer.findChild<NovaTerm::RenderScheduler*>();
        const auto readyTrace = QObject::connect(&core, &TerminalCore::historyDataReady, &host, [&] { trace("history_ready"); });
        const auto scheduleTrace = QObject::connect(scheduler, &NovaTerm::RenderScheduler::frameRequested,
            &host, [&] { trace("scheduler_handoff"); });
        std::vector<Request> requests(std::size_t(count), Request{});
        std::vector<double> frameGaps, correctFrameGaps, frameCpu, heartbeatLag, oldestBatchAge;
        qint64 lastFrame = 0, lastCorrect = 0, firstCorrect = 0, heartbeatAt = nowNs();
        quint64 previousFrames = before.framesRendered, previousCpu = before.cpuFrameNanoseconds;
        int handled = 0;
        renderer.dispatch = [&](const RequestEvent& input) {
            auto& request = requests.at(std::size_t(input.id));
            request.posted = input.posted; request.handled = nowNs();
            request.beforeRevision = renderer.renderStatistics().completedViewportRevision;
            if (input.gesture == ScrollGesture::Wheel) {
                const QPointF local(renderer.width() / 2., renderer.height() / 2.);
                QWheelEvent wheel(local, renderer.mapToGlobal(local.toPoint()), {}, QPoint(0, 120),
                                  Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
                QApplication::sendEvent(&renderer, &wheel);
            } else if (input.gesture == ScrollGesture::Page) {
                renderer.scrollLines(input.value);
            } else {
                bar.setValue(bar.maximum() - input.value);
            }
            request.offset = renderer.scrollOffset();
            const auto snapshot = core.scrollbackSnapshot();
            const auto index = renderer.historyDisplayRowCount() - request.offset;
            request.anchor = snapshot.lineInfoAt(index).id;
            ++handled;
            trace("request_handled");
        };
        const auto connection = QObject::connect(&renderer, &QRhiWidget::frameSubmitted, &host, [&] {
            const auto submitted = nowNs();
            trace("frame_submitted");
            const auto state = renderer.renderStatistics();
            if (state.framesRendered > previousFrames) {
                if (lastFrame) frameGaps.push_back(double(submitted - lastFrame) / 1e6);
                frameCpu.push_back(double(state.cpuFrameNanoseconds - previousCpu) / 1e6);
                lastFrame = submitted; previousFrames = state.framesRendered; previousCpu = state.cpuFrameNanoseconds;
            }
            int matched = -1;
            for (int i = 0; i < count; ++i) {
                const auto& request = requests[std::size_t(i)];
                if (request.handled && !request.completed && !request.coalesced
                    && request.anchor == state.completedHistoryAnchor
                    && state.completedViewportRevision > request.beforeRevision) matched = i;
            }
            if (matched >= 0) {
                qint64 oldest = requests[std::size_t(matched)].posted;
                requests[std::size_t(matched)].completed = submitted;
                for (int i = 0; i < matched; ++i) {
                    auto& prior = requests[std::size_t(i)];
                    if (prior.handled && !prior.completed && !prior.coalesced) {
                        oldest = (std::min)(oldest, prior.posted);
                        prior.coalesced = true; prior.supersededAt = submitted;
                    }
                }
                oldestBatchAge.push_back(double(submitted - oldest) / 1e6);
                if (!firstCorrect) firstCorrect = submitted;
                if (lastCorrect) correctFrameGaps.push_back(double(submitted - lastCorrect) / 1e6);
                lastCorrect = submitted;
            }
        });
        QTimer heartbeat;
        heartbeat.setTimerType(Qt::PreciseTimer); heartbeat.setInterval(5);
        QObject::connect(&heartbeat, &QTimer::timeout, &host, [&] {
            const auto tick = nowNs(); heartbeatLag.push_back((std::max)(0., double(tick - heartbeatAt) / 1e6 - 5.)); heartbeatAt = tick;
        });
        heartbeat.start();
        const auto targets = HistoryGuiBenchmark::jumpTargets(maximum, count, renderer.scrollOffset());
        int inputRejected = 0;
        quint64 inputBytes = 0;
        {
            Producer producer;
            if (stage == 3) {
                producer.worker = std::thread([&] {
                    auto deadline = Clock::now();
                    for (int i = 0; i < count && !producer.stop.load(); ++i) {
                        QByteArray bytes;
                        for (int line = 0; line < 16; ++line) bytes += logLine(nextRow++, mixed);
                        const auto accepted = core.writeInput(bytes);
                        inputBytes += quint64(accepted.acceptedBytes);
                        if (!accepted.fullyAccepted()) ++inputRejected;
                        QCoreApplication::postEvent(&renderer, new RequestEvent(i, ScrollGesture::Wheel, 0));
                        deadline += std::chrono::microseconds(16667);
                        std::this_thread::sleep_until(deadline);
                    }
                });
                await([&] { return failed || (handled == count && requests.back().completed != 0); }, 20000);
            } else {
                for (int i = 0; i < count; ++i) {
                    const auto issued = nowNs();
                    const auto gesture = stage == 0 ? ScrollGesture::Wheel : stage == 1 ? ScrollGesture::Page : ScrollGesture::Bar;
                    const int value = stage == 1 ? -core.rows() : targets[std::size_t(i)];
                    QCoreApplication::postEvent(&renderer, new RequestEvent(i, gesture, value));
                    await([&] { return failed || requests[std::size_t(i)].completed != 0; }, 10000);
                    while (nowNs() < issued + 16667000) events(1);
                }
            }
        }
        heartbeat.stop();
        renderer.dispatch = {};
        QObject::disconnect(connection);
        QObject::disconnect(readyTrace);
        QObject::disconnect(scheduleTrace);
        renderer.trace = {};
        require(!failed && inputRejected == 0, "GPU/input failure during measurement");
        const auto endNs = nowNs();
        const auto cpuMs = HistoryBenchmark::processCpuMs() - cpuBefore;
        const auto after = renderer.renderStatistics();
        const auto memoryAfter = runtime->statistics();
        std::vector<double> latency, queueDelay;
        QJsonArray raw;
        int coalesced = 0;
        for (const auto& request : requests) {
            require(request.handled != 0, "scroll event was not handled");
            if (request.completed) latency.push_back(double(request.completed - request.posted) / 1e6);
            else { require(request.coalesced, "scroll event disappeared without accounting"); ++coalesced; }
            queueDelay.push_back(double(request.handled - request.posted) / 1e6);
            raw.append(QJsonObject{{"anchor", double(request.anchor)}, {"offset", request.offset},
                {"posted_ms", double(request.posted - stageBegin) / 1e6},
                {"queued_to_handler_ms", double(request.handled - request.posted) / 1e6},
                {"event_to_frame_ms", request.completed ? QJsonValue(double(request.completed - request.posted) / 1e6) : QJsonValue()},
                {"coalesced", request.coalesced}, {"superseded_batch_age_ms", request.supersededAt ? QJsonValue(double(request.supersededAt - request.posted) / 1e6) : QJsonValue()}});
        }
        auto over = [](const std::vector<double>& gaps, double budget) { return std::count_if(gaps.begin(), gaps.end(), [&](double value) { return value > budget; }); };
        scenarios.append(QJsonObject{{"name", name}, {"closed_loop", stage != 3}, {"requests", count}, {"completed", int(latency.size())},
            {"coalesced", coalesced}, {"duration_ms", double(endNs - stageBegin) / 1e6}, {"process_cpu_ms", cpuMs},
            {"input_bytes", double(inputBytes)}, {"input_rejected", inputRejected},
            {"event_to_frame", distribution(latency)}, {"event_to_handler", distribution(queueDelay)},
            {"oldest_coalesced_batch_to_frame", distribution(oldestBatchAge)},
            {"correct_content_fps", lastCorrect > firstCorrect ? double(latency.size() - 1) * 1e9 / double(lastCorrect - firstCorrect) : 0.},
            {"frame_interval", distribution(frameGaps)}, {"correct_content_frame_interval", distribution(correctFrameGaps)},
            {"frame_cpu", distribution(frameCpu)}, {"heartbeat_lag", distribution(heartbeatLag)},
            {"frame_gaps_over_16_7_ms", double(over(frameGaps, 16.7))}, {"frame_gaps_over_33_3_ms", double(over(frameGaps, 33.3))},
            {"correct_content_gaps_over_16_7_ms", double(over(correctFrameGaps, 16.7))},
            {"correct_content_gaps_over_33_3_ms", double(over(correctFrameGaps, 33.3))},
            {"history_pending_frames", double(after.historyPendingFrames - before.historyPendingFrames)},
            {"cache_hits", double(memoryAfter.resources.cacheHits - memoryBefore.resources.cacheHits)},
            {"cache_misses", double(memoryAfter.resources.cacheMisses - memoryBefore.resources.cacheMisses)},
            {"busy_reads", double(memoryAfter.resources.busyReads - memoryBefore.resources.busyReads)},
            {"decode_errors", double(memoryAfter.resources.decodeErrors - memoryBefore.resources.decodeErrors)},
            {"resident_charge_before", double(memoryBefore.residentHistoryCharge)}, {"resident_charge_after", double(memoryAfter.residentHistoryCharge)},
            {"events", raw}, {"pipeline_trace", pipeline}});
        checkPixels(requests.back().anchor);
        QTextStream(stderr) << name << ": " << latency.size() << '/' << count << " responses, " << coalesced << " coalesced\n";
    }
    require(core.waitForIdle(), "final parser timeout");
    const auto final = runtime->statistics();
    require(final.resources.decodeErrors == 0 && core.scrollbackStatistics().storageErrors == 0, "history error");
    const auto memory = HistoryBenchmark::memory();
    QJsonObject result{{"schema_version", 1}, {"build_type", NOVATERM_GUI_BENCHMARK_BUILD_TYPE},
        {"compiler", NOVATERM_GUI_BENCHMARK_COMPILER}, {"font", renderer.font().toString()},
        {"pixel_check_scope", "independent final-state grabFramebuffer after each measured stage; readback redraws; not a capture of each timed frame"},
        {"profile", profile}, {"corpus", corpus}, {"input_rows", rows},
        {"samples_per_scenario", count}, {"platform", QGuiApplication::platformName()}, {"qt", qVersion()},
        {"rhi_api", int(renderer.api())}, {"screen_hz", host.windowHandle()->screen()->refreshRate()},
        {"dpr", renderer.devicePixelRatioF()}, {"columns", geometry.first}, {"rows", geometry.second},
        {"target_block_bytes", double(policy.targetBlockBytes)}, {"hot_bytes", double(policy.hotBytes)},
        {"prefill_wall_ms", prefillMs}, {"prefill_process_cpu_ms", prefillCpuMs},
        {"initial_retained_lines", double(initial.logicalLines)},
        {"initial_resident_charge", double(initialMemory.residentHistoryCharge)}, {"initial_compressed_blocks", double(initial.compressedBlocks)},
        {"rss", double(memory.rss)}, {"peak_rss", double(memory.peak)}, {"pixel_checks", pixelChecks},
        {"pixel_readback", distribution(pixelReadMs)}, {"queue_high_water", double(final.queueHighWater)},
        {"rejected_tasks", double(final.rejectedTasks)}, {"scenarios", scenarios}, {"verified", true},
        {"measurement_boundary", "synthetic event posted to completed content+glyph draw pass frameSubmitted; not compositor presentation"}};
    host.close();
    return result;
}
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    try {
        if (app.arguments().contains("--self-test")) {
            selfTest(); QTextStream(stdout) << "history GUI completed-viewport check passed\n";
        } else {
            const auto result = measure(app);
            const auto path = option(app.arguments(), "--output", "");
            const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Indented);
            if (path.isEmpty()) QTextStream(stdout) << bytes;
            else {
                QSaveFile output(path);
                require(output.open(QIODevice::WriteOnly), "cannot open JSON report");
                require(output.write(bytes) == bytes.size() && output.commit(), "cannot save JSON report");
            }
        }
        return 0;
    } catch (const std::exception& error) {
        QTextStream(stderr) << "history GUI benchmark: " << error.what() << '\n';
        return 1;
    }
}
