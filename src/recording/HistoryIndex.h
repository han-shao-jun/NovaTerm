#pragma once
#include "HistoryTypes.h"
#include <memory>

namespace NovaTerm::Recording {
// Rebuildable metadata only. Never advances committed high-water.
class HistoryIndex final {
public:
    struct Location { int chunk; qint64 offset; quint64 first; quint64 last; };
    HistoryIndex();
    ~HistoryIndex();
    bool open(const QString& captureDirectory);
    bool beginRebuild();
    bool add(const HistoryRecord&, int chunk, qint64 offset, const QByteArray& hash);
    bool commit();
    bool begin();
    std::vector<Location> locate(quint64 first, quint64 last, size_t limit);
    QString error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
