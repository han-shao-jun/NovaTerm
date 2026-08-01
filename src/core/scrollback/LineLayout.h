#pragma once

#include "ScrollbackSnapshot.h"

#include <QObject>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

namespace NovaTerm {

struct DisplayLine
{
    LineId lineId{0};
    qsizetype startCell{0};
    qsizetype endCell{0};
    qsizetype wrapIndex{0};
    bool hardBreak{false};
};

struct ViewportSnapshot
{
    quint64 sourceVersion{0};
    quint64 generation{0};
    int columns{0};
    QVector<DisplayLine> rows;
};

struct ReflowBatch
{
    quint64 sourceVersion{0};
    quint64 generation{0};
    qsizetype logicalStart{0};
    qsizetype logicalProcessed{0};
    qsizetype physicalRows{0};
    QVector<DisplayLine> rows;
    bool completed{false};
    bool cancelled{false};
    QString error;
};

class LineLayout
{
public:
    static QVector<DisplayLine> wrapLine(
        const LogicalLine& line, int columns,
        const std::function<bool()>& cancelled = {});
    static ViewportSnapshot viewport(const ScrollbackSnapshot& snapshot,
                                     LineId anchorLine, qsizetype wrapOffset,
                                     int columns, qsizetype rowCount,
                                     qsizetype trailingCache = 32,
                                     quint64 generation = 0);
};

// Runs complete historical reflow away from UI/parser threads and reports
// progress in bounded batches. A newer generation supersedes older work.
class ReflowEngine final : public QObject
{
    Q_OBJECT
public:
    explicit ReflowEngine(QObject* parent = nullptr);
    ~ReflowEngine() override;

    void request(ScrollbackSnapshot snapshot, int columns,
                 quint64 generation, qsizetype batchLines = 1024);
    void cancel(quint64 generation);

signals:
    void batchReady(const NovaTerm::ReflowBatch& batch);

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace NovaTerm

Q_DECLARE_METATYPE(NovaTerm::DisplayLine)
Q_DECLARE_METATYPE(NovaTerm::ViewportSnapshot)
Q_DECLARE_METATYPE(NovaTerm::ReflowBatch)
