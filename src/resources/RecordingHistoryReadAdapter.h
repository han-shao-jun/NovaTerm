#pragma once
#include "recording/HistoryService.h"
#include "historysearch/HistorySearchTypes.h"
#include <QPointer>
namespace NovaTerm::Recording {
// Concrete recording adapter used only by composition roots.
inline HistoryReadPort recordingReadPort(HistoryService* service) {
    const QPointer<HistoryService> source(service);
    return {[source](HistoryCaptureId id) -> std::optional<HistoryResource> {
        if (source) for (const auto& capture : source->captures()) if (capture.id == id) return HistoryResource{capture, 1};
        return std::nullopt;
    }, [source](HistoryCaptureId id, HistoryRange range, auto done) {
        return source && source->readCommitted(id, range, std::move(done));
    }};
}
}
