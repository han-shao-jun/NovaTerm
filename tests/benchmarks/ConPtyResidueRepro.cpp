// Temporary diagnostic tool: drives a real cmd.exe + Clink ConPTY session to
// reproduce the post-scroll line residue, then grabs the renderer.
#include "core/terminal/TerminalCore.h"
#include "renderer/TerminalRenderer.h"
#include "transport/ITransport.h"
#include "transport/LocalShellTransport.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
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

void runScenario(TerminalCore& core, TerminalRenderer& renderer,
                 LocalShellTransport& transport)
{
    QTextStream out(stdout);
    const auto write = [&](const QByteArray& data) {
        transport.write(data);
        waitEvents(120);
        core.waitForIdle(2000);
    };

    // Prompt banner appears; wait for it.
    waitEvents(1500);

    // 1) Type a long command (Clink redraws the input line on every key).
    out << "typing long command...\n";
    const QByteArray longCommand =
        QByteArrayLiteral("echo VERY_LONG_OUTPUT_LINE_")
        + QByteArray(40, 'X');
    write(longCommand);
    waitEvents(400);

    // 2) Shorten it with backspaces (Clink redraws the shorter line).
    out << "backspacing...\n";
    write(QByteArray(30, '\b'));
    waitEvents(400);

    // 3) Run it: output + scroll + new prompt.
    out << "enter...\n";
    write(QByteArrayLiteral("\r"));
    waitEvents(800);

    // 4) Type a long command again, then replace it with a shorter one via
    //    Ctrl+U (clear line) and retype.
    write(QByteArrayLiteral("echo tailresidue-") + QByteArray(30, 'Y'));
    waitEvents(300);
    write(QByteArrayLiteral("\x03"));  // Ctrl+C cancels
    waitEvents(400);
    write(QByteArrayLiteral("echo short"));
    waitEvents(300);
    write(QByteArrayLiteral("\r"));
    waitEvents(800);

    const bool converged = waitUntil([&]() {
        return renderer.renderProgress().lastRenderedRevision
            == core.modelRevision();
    }, 5000);
    out << "converged=" << (converged ? "yes" : "NO") << '\n';
    const auto stats = renderer.renderStatistics();
    out << "framesRendered=" << stats.framesRendered
        << " rowsRebuilt=" << stats.rowsRebuilt
        << " scrollbackLines=" << core.scrollbackLineCount() << '\n';
    const auto model = core.snapshot();
    QString modelText;
    for (int row = 0; row < model.rows; ++row) {
        QString line;
        for (int col = 0; col < model.columns; ++col) {
            const auto* cell = model.cellAt(row, col);
            if (cell && cell->chars[0] != 0 && cell->chars[0] != ' ')
                line += QChar(ushort(cell->chars[0]));
            else
                line += QLatin1Char(' ');
        }
        if (!line.trimmed().isEmpty())
            modelText += QStringLiteral("r%1: %2\n").arg(row).arg(line);
    }
    QFile modelFile(QStringLiteral("conpty_model.txt"));
    if (modelFile.open(QIODevice::WriteOnly))
        modelFile.write(modelText.toUtf8());
    out << "model rows with text: "
        << modelText.count(QLatin1Char('\n')) << '\n';
    waitEvents(300);
    const QImage grab = renderer.grab().toImage();
    const QString path = QStringLiteral("conpty_residue.png");
    out << "grabbed=" << grab.size().width() << 'x' << grab.size().height()
        << " saved=" << grab.save(path) << " -> " << path << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    TerminalCore core(100, 30);
    if (!core.waitForIdle())
        return 4;
    TerminalRenderer renderer(&core);
    renderer.setConservativeLiveScrollRendering(true);  // Windows default
    renderer.resize(1000, 600);
    renderer.show();
    renderer.raise();
    renderer.activateWindow();
    waitEvents(600);
    if (!renderer.windowHandle() || !renderer.windowHandle()->isExposed()) {
        QTextStream(stderr) << "widget not exposed\n";
        return 2;
    }

    LocalShellTransport transport;
    transport.setShellProgram(QStringLiteral("cmd.exe"));
    const QString clinkBat = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("clink.bat"));
    if (!QFileInfo::exists(clinkBat)) {
        QTextStream(stderr) << "clink.bat missing: " << clinkBat << '\n';
        return 3;
    }
    transport.setShellArgs({
        QStringLiteral("/k"),
        QDir::toNativeSeparators(clinkBat),
        QStringLiteral("inject")
    });
    transport.resizeTerminal(100, 30);

    QObject::connect(&transport, &ITransport::readyRead, &core,
                     [&core](const QByteArray& data) {
        if (!data.isEmpty())
            core.writeInput(data);
    });
    QObject::connect(&core, &TerminalCore::outputData, &transport,
                     [&transport](const QByteArray& data) {
        if (transport.isConnected())
            transport.write(data);
    });

    if (!transport.connectToHost()) {
        QTextStream(stderr) << "connect failed: "
                            << transport.errorString() << '\n';
        return 5;
    }
    core.setScrollbackLimit(1000);

    runScenario(core, renderer, transport);
    transport.disconnect();
    renderer.close();
    return 0;
}
