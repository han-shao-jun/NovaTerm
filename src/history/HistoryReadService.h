#pragma once
#include "HistoryQueryTypes.h"
#include <QObject>
#include <map>

namespace NovaTerm::Recording {
class HistoryReadService final : public QObject {
public:
    using ValidateContext = std::function<bool(const ResourceContext&)>;
    HistoryReadService(HistorySourceReader&, HistoryAuthorizationStore&, ValidateContext, QObject* parent = nullptr);
    std::vector<CaptureDescriptor> list(const ResourceContext&) const;
    void readAsync(HistoryReadRequest, ResourceContext, std::function<void(HistoryReadPage)>, Cancellation = {});
    std::optional<ResourceGrant> authorize(const ResourceContext&, HistoryCaptureId) const;
    std::optional<CaptureDescriptor> descriptor(HistoryCaptureId) const;
    HistorySourceReader& source() const { return _source; }
    static constexpr size_t MaxCursors = 128;
private:
    struct Cursor {
        ResourceContext context;
        HistoryCaptureId capture;
        quint64 version, highWater, record;
        quint32 offset;
        std::chrono::steady_clock::time_point expires;
    };
    HistorySourceReader& _source;
    HistoryAuthorizationStore& _authorization;
    ValidateContext _validate;
    std::map<QString, Cursor> _cursors;
    size_t _active{0};
};
}
