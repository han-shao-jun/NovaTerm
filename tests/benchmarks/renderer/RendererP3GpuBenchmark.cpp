#include "core/terminal/TerminalCore.h"
#include "renderer/TerminalRenderer.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTextStream>
#include <QTimer>
#include <QWindow>

namespace {

void waitForEvents(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

TerminalRenderer::RenderStatistics delta(
    const TerminalRenderer::RenderStatistics& after,
    const TerminalRenderer::RenderStatistics& before)
{
    TerminalRenderer::RenderStatistics result = after;
    result.rowsRebuilt -= before.rowsRebuilt;
    result.commandsGenerated -= before.commandsGenerated;
    result.commandGenerationNanoseconds -=
        before.commandGenerationNanoseconds;
    result.cpuFrameNanoseconds -= before.cpuFrameNanoseconds;
    result.gpuUploadBytes -= before.gpuUploadBytes;
    result.drawCalls -= before.drawCalls;
    result.vertexBufferReallocations -= before.vertexBufferReallocations;
    result.revisionPromotedFullFrames -=
        before.revisionPromotedFullFrames;
    result.revisionRecoveredRows -= before.revisionRecoveredRows;
    result.framesRendered -= before.framesRendered;
    result.cpuFramesOverBudget -= before.cpuFramesOverBudget;
    return result;
}

void printScenario(QTextStream& output, const QString& name,
                   const TerminalRenderer::RenderStatistics& value)
{
    output << name
           << ": frames=" << value.framesRendered
           << ", rows_rebuilt=" << value.rowsRebuilt
           << ", commands=" << value.commandsGenerated
           << ", upload_bytes=" << value.gpuUploadBytes
           << ", draw_calls=" << value.drawCalls
           << ", buffer_reallocations=" << value.vertexBufferReallocations
           << ", revision_full_promotions="
           << value.revisionPromotedFullFrames
           << ", revision_recovered_rows=" << value.revisionRecoveredRows
           << ", cpu_over_budget=" << value.cpuFramesOverBudget << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    int durationMs = 5000;
    const QStringList arguments = application.arguments();
    const int option = arguments.indexOf(QStringLiteral("--duration-ms"));
    if (option >= 0 && option + 1 < arguments.size()) {
        bool ok = false;
        const int requested = arguments[option + 1].toInt(&ok);
        if (ok && requested >= 1000)
            durationMs = requested;
    }

    TerminalCore core(120, 40);
    TerminalRenderer renderer(&core);
    bool renderFailed = false;
    QObject::connect(&renderer, &QRhiWidget::renderFailed,
                     [&renderFailed]() { renderFailed = true; });
    renderer.setTargetRefreshRate(60);
    renderer.resize(1152, 760);
    renderer.show();
    waitForEvents(750);
    if (!renderer.windowHandle() || !renderer.windowHandle()->isExposed()
        || renderFailed) {
        QTextStream(stderr)
            << "P3 GPU benchmark could not expose a working QRhiWidget\n";
        return 2;
    }
    core.waitForIdle();
    waitForEvents(250);

    // Position the cursor in a separate publication so the measured update
    // contains one printable Cell damage rather than a cursor-addressing
    // control sequence and its overlay transition.
    core.writeInput(QByteArrayLiteral("\x1b[1;1H"));
    if (!core.waitForIdle())
        return 3;
    waitForEvents(100);

    const auto beforeSingle = renderer.renderStatistics();
    core.writeInput(QByteArrayLiteral("X"));
    if (!core.waitForIdle())
        return 3;
    waitForEvents(100);
    const auto afterSingle = renderer.renderStatistics();
    const auto single = delta(afterSingle, beforeSingle);

    const auto beforeFull = afterSingle;
    renderer.setColorScheme(renderer.colorScheme());
    waitForEvents(100);
    const auto afterFull = renderer.renderStatistics();
    const auto full = delta(afterFull, beforeFull);

    quint64 line = 0;
    QTimer outputTimer;
    outputTimer.setTimerType(Qt::PreciseTimer);
    outputTimer.setInterval(1);
    QObject::connect(&outputTimer, &QTimer::timeout, [&core, &line]() {
        const QByteArray data = QByteArrayLiteral("\r\nP3-GPU-")
            + QByteArray::number(++line);
        core.writeInput(data);
    });
    const auto beforeContinuous = afterFull;
    QElapsedTimer continuousTimer;
    continuousTimer.start();
    outputTimer.start();
    waitForEvents(durationMs);
    outputTimer.stop();
    const qint64 continuousElapsedMs = continuousTimer.elapsed();
    const auto afterContinuous = renderer.renderStatistics();
    const auto continuous = delta(afterContinuous, beforeContinuous);
    if (!core.waitForIdle(10000))
        return 4;
    waitForEvents(250);
    const auto final = renderer.renderStatistics();

    QTextStream output(stdout);
    output << "NovaTerm P3 QRhi GPU benchmark\n"
           << "api=" << qEnvironmentVariable("NOVATERM_RHI_API", "default")
           << ", geometry=" << core.columns() << 'x' << core.rows()
           << ", duration_ms=" << continuousElapsedMs << '\n';
    printScenario(output, QStringLiteral("single_cell"), single);
    printScenario(output, QStringLiteral("forced_full"), full);
    printScenario(output, QStringLiteral("continuous_output"), continuous);
    const double fps = continuousElapsedMs > 0
        ? double(continuous.framesRendered) * 1000.0
              / double(continuousElapsedMs)
        : 0.0;
    output << "continuous_fps=" << fps
           << ", cpu_ns[p50/p95/p99]="
           << final.cpuFrameP50Nanoseconds << '/'
           << final.cpuFrameP95Nanoseconds << '/'
           << final.cpuFrameP99Nanoseconds
           << ", published_revision=" << core.modelRevision()
           << ", rendered_revision=" << final.lastRenderedRevision
           << ", revision_converged="
           << (core.modelRevision() == final.lastRenderedRevision ? "true"
                                                                 : "false")
           << '\n';
    renderer.close();
    return renderFailed ? 5 : 0;
}
