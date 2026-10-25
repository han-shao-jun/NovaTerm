#pragma once
#include "HistoryTypes.h"
#include <functional>
#include <memory>

namespace NovaTerm::Recording {
// Synchronous I/O boundary: call only on a History worker, never the input thread.
class HistoryChunkStore final {
public:
    using FaultProbe = std::function<bool(IoPoint)>;
    HistoryChunkStore(QString historyRoot, CaptureDescriptor, StorageOptions = {}, FaultProbe = {});
    ~HistoryChunkStore();
    bool open();
    bool append(const std::vector<HistoryRecord>&);
    bool commit();
    bool seal(const QString& reason);
    bool recover();
    void setCaptureLimit(quint64 bytes);
    ReadResult readCommitted(HistoryRange);
    CaptureDescriptor descriptor() const;
    QString error() const;
    QString directory() const;
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
