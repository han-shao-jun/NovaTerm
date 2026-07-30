#include "core/terminal/TerminalCore.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTextStream>
#include <QThread>

#include <algorithm>

namespace {

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

void enqueueWithoutLoss(TerminalCore& core, const QByteArray& data)
{
    while (!core.writeInput(data))
        QThread::yieldCurrentThread();
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

    QElapsedTimer parserTimer;
    parserTimer.start();
    qint64 parsedBytes = 0;
    while (parsedBytes < targetBytes) {
        enqueueWithoutLoss(core, parserChunk);
        parsedBytes += parserChunk.size();
    }
    if (!core.waitForIdle(60'000))
        return 2;
    const qint64 parserNanoseconds = parserTimer.nsecsElapsed();

    const QByteArray scrollbackLine = QByteArrayLiteral("scrollback baseline line\r\n");
    QElapsedTimer scrollbackTimer;
    scrollbackTimer.start();
    for (qint64 line = 0; line < targetLines; ++line)
        enqueueWithoutLoss(core, scrollbackLine);
    if (!core.waitForIdle(60'000))
        return 3;
    const qint64 scrollbackNanoseconds = scrollbackTimer.nsecsElapsed();

    QTextStream output(stdout);
    output.setRealNumberNotation(QTextStream::FixedNotation);
    output.setRealNumberPrecision(2);
    output << "NovaTerm core benchmark\n"
           << "parser.bytes=" << parsedBytes << '\n'
           << "parser.elapsed_ms=" << parserNanoseconds / 1'000'000.0 << '\n'
           << "parser.throughput_mib_s=" << mibPerSecond(parsedBytes, parserNanoseconds) << '\n'
           << "scrollback.requested_lines=" << targetLines << '\n'
           << "scrollback.stored_lines=" << core.scrollbackLineCount() << '\n'
           << "scrollback.elapsed_ms=" << scrollbackNanoseconds / 1'000'000.0 << '\n';

    return 0;
}
