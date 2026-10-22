#pragma once
#include "EvidenceTypes.h"

namespace NovaTerm::Evidence {
struct SourceReadResult {
    Error error = Error::None;
    QByteArray bytes;
    quint64 generation = 0;
    QString sourceCompleteness;
    quint64 projectionVersion = 0;
};
// Adapter owns source lifetime and validates the resource generation/existence on every read.
// Completion must occur on the calling owner's thread, exactly once. No GUI disk I/O.
class EvidenceSourceReader {
public:
    virtual ~EvidenceSourceReader() = default;
    virtual void readAsync(const ResourceBinding&, const QVector<RawFragment>&,
                           qsizetype maxBytes, std::function<void(SourceReadResult)>) const = 0;
};
}
