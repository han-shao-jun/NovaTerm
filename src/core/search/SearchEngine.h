#pragma once

#include "core/scrollback/ScrollbackSnapshot.h"

#include <QObject>
#include <QRegularExpression>
#include <QString>
#include <QVector>

#include <memory>

namespace NovaTerm {

struct SearchRequest
{
    QString query;
    bool caseSensitive{false};
    bool regularExpression{false};
    bool wholeWord{false};
    LineId firstLine{0};
    LineId lastLine{0};
    quint64 generation{0};
    qsizetype resultBatchSize{128};
    qsizetype maximumResults{100'000};
};

struct SearchMatch
{
    LineId lineId{0};
    qsizetype startCell{0};
    qsizetype endCell{0};
};

struct SearchBatch
{
    quint64 generation{0};
    quint64 sourceVersion{0};
    QVector<SearchMatch> matches;
    qsizetype scannedLines{0};
    qsizetype totalLines{0};
    bool completed{false};
    bool cancelled{false};
    QString error;
};

class SearchEngine final : public QObject
{
    Q_OBJECT
public:
    explicit SearchEngine(QObject* parent = nullptr);
    ~SearchEngine() override;

    void search(ScrollbackSnapshot snapshot, SearchRequest request);
    void cancel(quint64 generation);

signals:
    void resultsReady(const NovaTerm::SearchBatch& batch);

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace NovaTerm

Q_DECLARE_METATYPE(NovaTerm::SearchRequest)
Q_DECLARE_METATYPE(NovaTerm::SearchMatch)
Q_DECLARE_METATYPE(NovaTerm::SearchBatch)
