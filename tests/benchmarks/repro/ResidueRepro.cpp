// Temporary diagnostic tool: reproduces the post-scroll line residue.
#include "core/terminal/TerminalCore.h"
#include "renderer/TerminalRenderer.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QTextStream>
#include <QTimer>
#include <QWindow>

namespace {

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

bool writeAndDrain(TerminalCore& core, const QByteArray& input,
                   int timeoutMilliseconds)
{
    const auto result = core.writeInput(input);
    if (!result.fullyAccepted())
        return false;
    if (!core.waitForIdle(timeoutMilliseconds))
        return false;
    return true;
}

QByteArray longRow(char seed, int cols)
{
    QByteArray row;
    for (int c = 0; c < cols - 4; ++c)
        row += char('a' + (seed % 26));
    return row;
}

void runScenario(int mode, TerminalCore& core, TerminalRenderer& renderer)
{
    const int rows = core.rows();
    const int cols = core.columns();
    QTextStream out(stdout);
    out << "── mode=" << mode
        << " (conservative=" << (mode == 0 ? "fast" : "full") << ")\n";

    // 1) Fill the screen with long lines.
    QByteArray fill;
    for (int row = 0; row < rows; ++row) {
        fill += "r";
        fill += QByteArray::number(row);
        fill += '-';
        fill += longRow(char(row), cols);
        fill += "\r\n";
    }
    if (!writeAndDrain(core, fill, 5000))
        out << "fill failed\n";
    waitEvents(150);

    // 2) Stress: repeatedly write a long prompt on the bottom row, scroll,
    //    then shorten the same row in place (the ConPTY tail-residue pattern).
    for (int iteration = 0; iteration < 40; ++iteration) {
        const QByteArray bottom = "\x1b["
            + QByteArray::number(rows) + ";1H";
        // Long prompt with a long tail.
        QByteArray longPrompt = "PS> ";
        for (int c = 0; c < 34 + (iteration % 7); ++c)
            longPrompt += char('x');
        // Short prompt + erase tail, issued together with a scroll in the
        // same parser publication.
        const QByteArray shortPrompt = "PS> \x1b[0K";
        const QByteArray batch = (iteration % 3 == 0)
            ? QByteArrayLiteral("\r\n") + bottom + shortPrompt
            : QByteArrayLiteral("\r\n") + bottom + longPrompt
                + QByteArrayLiteral("\r\n") + bottom + shortPrompt;
        if (!writeAndDrain(core, batch, 5000)) {
            out << "iteration " << iteration << " failed\n";
            break;
        }
        waitEvents(10);
    }
    waitEvents(250);

    const bool converged = waitUntil([&]() {
        return renderer.renderProgress().lastRenderedRevision
            == core.modelRevision();
    }, 5000);
    out << "converged=" << (converged ? "yes" : "NO") << '\n';
    waitEvents(300);
    const QImage grab = renderer.grab().toImage();
    const QString path = QStringLiteral("residue_mode%1.png").arg(mode);
    const bool saved = grab.save(path);
    out << "grabbed=" << grab.size().width() << 'x' << grab.size().height()
        << " saved=" << saved << " -> " << path << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    for (int mode = 0; mode < 2; ++mode) {
        TerminalCore core(60, 12);
        if (!core.waitForIdle())
            return 4;
        TerminalRenderer renderer(&core);
        renderer.setConservativeLiveScrollRendering(mode == 1);
        renderer.resize(640, 260);
        renderer.show();
        renderer.raise();
        renderer.activateWindow();
        waitEvents(600);
        if (!renderer.windowHandle() || !renderer.windowHandle()->isExposed()) {
            QTextStream(stderr) << "widget not exposed in mode " << mode << '\n';
            return 2;
        }
        runScenario(mode, core, renderer);
        renderer.close();
        waitEvents(200);
    }
    return 0;
}
