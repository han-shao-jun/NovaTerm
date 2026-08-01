#include "core/terminal/TerminalCore.h"
#include "renderer/RenderCommandBuffer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSysInfo>
#include <QTextStream>

#include <algorithm>

namespace {

constexpr int Columns = 120;
constexpr int Rows = 40;
constexpr int VerticesPerQuad = 6;
constexpr int GpuVertexBytes = 8 * int(sizeof(float));

struct Distribution
{
    qint64 p50{0};
    qint64 p95{0};
    qint64 p99{0};
};

struct PipelineResult
{
    Distribution cpu;
    quint64 commands{0};
    quint64 modeledUploadBytes{0};
};

Distribution distribution(QVector<qint64> samples)
{
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&samples](double value) {
        const qsizetype index = std::min<qsizetype>(
            samples.size() - 1,
            qsizetype(value * double(samples.size() - 1)));
        return samples[index];
    };
    return {percentile(0.50), percentile(0.95), percentile(0.99)};
}

void populateDenseScreen(TerminalCore& core)
{
    QByteArray input;
    input.reserve(Rows * (Columns + 12));
    for (int row = 1; row <= Rows; ++row) {
        input += QStringLiteral("\x1b[%1;1H").arg(row).toUtf8();
        // Keep the final column empty so libvterm does not wrap/scroll the
        // bottom row while still exercising a dense terminal workload.
        input += QByteArray(Columns - 1, char('A' + (row % 26)));
    }
    const auto write = core.writeInput(input);
    if (!write.fullyAccepted() || !core.waitForIdle())
        qFatal("failed to populate P3 benchmark screen");
}

quint64 rebuildRows(const NovaTerm::RendererSnapshot& snapshot,
                    const QVector<bool>& dirtyRows,
                    NovaTerm::RenderCommandBuffer& buffer)
{
    quint64 commandCount = 0;
    NovaTerm::RenderCommand background;
    background.type = NovaTerm::RenderCommandType::BackgroundRect;
    NovaTerm::RenderCommand glyph;
    glyph.type = NovaTerm::RenderCommandType::GlyphInstance;

    for (int row = 0; row < snapshot.rows; ++row) {
        if (!dirtyRows.value(row))
            continue;
        QVector<NovaTerm::RenderCommand> backgrounds;
        QVector<NovaTerm::RenderCommand> contents;
        backgrounds.reserve(snapshot.columns);
        contents.reserve(snapshot.columns);
        for (int column = 0; column < snapshot.columns; ++column) {
            backgrounds.push_back(background);
            const NovaTerm::Cell* cell = snapshot.cellAt(row, column);
            if (cell && !cell->isWideContinuation()
                && cell->chars[0] != 0 && cell->chars[0] != ' ') {
                contents.push_back(glyph);
            }
        }
        commandCount += quint64(backgrounds.size() + contents.size());
        buffer.replaceRow(row, std::move(backgrounds),
                          std::move(contents), 1);
    }
    return commandCount;
}

PipelineResult benchmarkPipeline(TerminalCore& core, int dirtyRowCount,
                                 int iterations)
{
    NovaTerm::RenderCommandBuffer buffer;
    buffer.resize(Rows, Columns);
    QVector<qint64> samples;
    samples.reserve(iterations);
    quint64 commandCount = 0;

    for (int iteration = -200; iteration < iterations; ++iteration) {
        QVector<bool> dirtyRows(Rows, dirtyRowCount == Rows);
        if (dirtyRowCount != Rows)
            dirtyRows[iteration < 0 ? 0 : iteration % Rows] = true;

        QElapsedTimer timer;
        timer.start();
        const NovaTerm::RendererSnapshot snapshot =
            core.rendererSnapshot(dirtyRows, 0);
        commandCount = rebuildRows(snapshot, dirtyRows, buffer);
        const qint64 elapsed = timer.nsecsElapsed();
        if (iteration >= 0)
            samples.push_back(elapsed);
    }

    return {
        distribution(std::move(samples)),
        commandCount,
        commandCount * VerticesPerQuad * GpuVertexBytes
    };
}

void printMetric(QTextStream& output, const QString& name,
                 const PipelineResult& value, int dirtyRows)
{
    output << name
           << ": dirtyRows=" << dirtyRows
           << ", commands=" << value.commands
           << ", cpu_ns[p50/p95/p99]=" << value.cpu.p50 << '/'
           << value.cpu.p95 << '/' << value.cpu.p99
           << ", modeled_upload_bytes=" << value.modeledUploadBytes << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    int iterations = 20000;
    const QStringList arguments = application.arguments();
    const int option = arguments.indexOf(QStringLiteral("--iterations"));
    if (option >= 0 && option + 1 < arguments.size()) {
        bool ok = false;
        const int requested = arguments[option + 1].toInt(&ok);
        if (ok && requested >= 100)
            iterations = requested;
    }

    TerminalCore core(Columns, Rows);
    populateDenseScreen(core);
    const int fullIterations = std::max(500, iterations / Rows);
    const PipelineResult oneRow = benchmarkPipeline(core, 1, iterations);
    const PipelineResult fullFrame =
        benchmarkPipeline(core, Rows, fullIterations);

    QTextStream output(stdout);
    output << "NovaTerm P3 incremental rendering support benchmark\n"
           << "scope=CPU rendererSnapshot + dirty-row Cell traversal + "
              "RenderCommandBuffer replacement; GPU upload is modeled from "
              "the fixed-slot vertex format (no GPU timing)\n"
           << "grid=" << Columns << 'x' << Rows
           << ", qt=" << QT_VERSION_STR
           << ", os=" << QSysInfo::prettyProductName()
           << ", cpu_arch=" << QSysInfo::currentCpuArchitecture() << '\n';
    printMetric(output, QStringLiteral("single_row"), oneRow, 1);
    printMetric(output, QStringLiteral("full_frame"), fullFrame, Rows);
    const double reduction = fullFrame.modeledUploadBytes > 0
        ? 100.0 * (1.0 - double(oneRow.modeledUploadBytes)
                            / double(fullFrame.modeledUploadBytes))
        : 0.0;
    output << "upload_reduction=" << reduction << "%\n";
    return 0;
}
