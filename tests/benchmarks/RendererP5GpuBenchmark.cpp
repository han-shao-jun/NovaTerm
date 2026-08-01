#include "core/terminal/TerminalCore.h"
#include "renderer/TerminalRenderer.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFont>
#include <QTextStream>
#include <QTimer>
#include <QWindow>

#include <algorithm>

namespace {

void waitEvents(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
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
        << ", revision_recovered=" << s.revisionRecoveredRows << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    int durationMs = 5000;
    const QStringList args = app.arguments();
    const int durationOption = args.indexOf(QStringLiteral("--duration-ms"));
    if (durationOption >= 0 && durationOption + 1 < args.size())
        durationMs = std::max(1000, args[durationOption + 1].toInt());

    TerminalCore core(119, 40);
    TerminalRenderer renderer(&core);
    bool failed = false;
    QObject::connect(&renderer, &QRhiWidget::renderFailed,
                     [&failed]() { failed = true; });
    renderer.resize(1152, 760);
    renderer.setTargetRefreshRate(60);
    renderer.show();
    waitEvents(750);
    if (failed || !renderer.windowHandle()
        || !renderer.windowHandle()->isExposed()) {
        QTextStream(stderr) << "P5 QRhi benchmark could not expose widget\n";
        return 2;
    }

    auto mark = renderer.renderStatistics();
    core.writeInput(QByteArrayLiteral("\x1b[1;1HX"));
    core.waitForIdle(); waitEvents(100);
    auto now = renderer.renderStatistics();
    const auto single = delta(now, mark); mark = now;

    core.writeInput(QByteArrayLiteral("\x1b[2;1HAAAAAAAAAAAAAAAA"));
    core.waitForIdle(); waitEvents(100);
    mark = renderer.renderStatistics();
    core.writeInput(QByteArrayLiteral("\x1b[3;1HAAAAAAAAAAAAAAAA"));
    core.waitForIdle(); waitEvents(100);
    now = renderer.renderStatistics();
    const auto warm = delta(now, mark); mark = now;

    const QByteArray complexCorpus = QStringLiteral(
        "中文 e\u0301 한글 日本語 ┌─┐ \ue0b0 👩‍💻 🧑🏽‍🚀").toUtf8();
    core.writeInput(QByteArrayLiteral("\x1b[4;1H") + complexCorpus);
    core.waitForIdle(); waitEvents(150);
    now = renderer.renderStatistics();
    const auto complex = delta(now, mark); mark = now;

    core.writeInput(QByteArrayLiteral("\x1b[5;1H") + complexCorpus);
    core.waitForIdle(); waitEvents(150);
    now = renderer.renderStatistics();
    const auto complexWarm = delta(now, mark); mark = now;

    core.writeInput(QByteArrayLiteral("\x1b[6;5H"));
    core.waitForIdle(); waitEvents(100);
    now = renderer.renderStatistics();
    const auto overlay = delta(now, mark); mark = now;

    quint64 line = 0;
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    timer.setInterval(17);
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        core.writeInput(QByteArrayLiteral("\r\nP5-scroll-")
                        + QByteArray::number(++line));
    });
    QVector<quint64> rowSamples;
    quint64 sampledRows = renderer.renderStatistics().rowsRebuilt;
    QTimer sampler;
    sampler.setTimerType(Qt::PreciseTimer);
    sampler.setInterval(17);
    QObject::connect(&sampler, &QTimer::timeout, [&]() {
        const quint64 current = renderer.renderStatistics().rowsRebuilt;
        rowSamples.push_back(current - sampledRows);
        sampledRows = current;
    });
    QElapsedTimer elapsed;
    elapsed.start(); timer.start(); sampler.start();
    waitEvents(durationMs); timer.stop(); sampler.stop();
    const qint64 scrollMs = elapsed.elapsed();
    core.waitForIdle(10000); waitEvents(250);
    now = renderer.renderStatistics();
    const auto scroll = delta(now, mark); mark = now;

    renderer.setColorScheme(renderer.colorScheme()); waitEvents(120);
    now = renderer.renderStatistics();
    const auto full = delta(now, mark); mark = now;

    renderer.resize(1060, 700); waitEvents(150);
    now = renderer.renderStatistics();
    const auto resize = delta(now, mark); mark = now;

    // Keep both grayscale and color pages visible while exercising resource
    // invalidation so a deferred second-page upload must complete by itself.
    core.writeInput(QByteArrayLiteral("\x1b[1;1H") + complexCorpus);
    core.waitForIdle(); waitEvents(150);
    mark = renderer.renderStatistics();
    QFont changed = renderer.font();
    changed.setPixelSize(changed.pixelSize() + 1);
    renderer.setFont(changed);
    core.waitForIdle(10000);
    waitEvents(250);
    now = renderer.renderStatistics();
    const auto font = delta(now, mark);

    const auto final = renderer.renderStatistics();
    std::sort(rowSamples.begin(), rowSamples.end());
    const quint64 rowsP95 = rowSamples.isEmpty() ? 0
        : rowSamples[qsizetype(0.95 * double(rowSamples.size() - 1))];
    QTextStream out(stdout);
    out << "NovaTerm P5 QRhi benchmark\napi="
        << qEnvironmentVariable("NOVATERM_RHI_API", "default")
        << ", grid=" << core.columns() << 'x' << core.rows()
        << ", duration_ms=" << scrollMs << '\n';
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
        << ", scroll_rows_p95=" << rowsP95
        << ", cpu_ns[p50/p95/p99]=" << final.cpuFrameP50Nanoseconds << '/'
        << final.cpuFrameP95Nanoseconds << '/'
        << final.cpuFrameP99Nanoseconds
        << ", buffer_current=" << final.bufferCurrentBytes
        << ", buffer_peak=" << final.bufferPeakBytes
        << ", memory_peak=" << final.memoryPeakBytes
        << ", published_revision=" << core.modelRevision()
        << ", rendered_revision=" << final.lastRenderedRevision
        << ", converged="
        << (core.modelRevision() == final.lastRenderedRevision ? "true" : "false")
        << '\n';
    renderer.close();
    return failed ? 3 : 0;
}
