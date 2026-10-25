#pragma once
#include "HistoryAuthorizationStore.h"
#include <QObject>
#include <memory>
namespace NovaTerm::Recording {
// Call on the application thread. The read port performs storage I/O asynchronously.
class HistoryReadService final : public QObject {
public:
    using ReadCallback = std::function<void(HistoryReadPage)>;
    explicit HistoryReadService(HistoryReadPort, HistoryAuthorizationStore&, QObject* parent = nullptr);
    ~HistoryReadService() override;
    HistoryReadService(const HistoryReadService&) = delete;
    HistoryReadService& operator=(const HistoryReadService&) = delete;
    HistoryReadService(HistoryReadService&&) = delete;
    HistoryReadService& operator=(HistoryReadService&&) = delete;
    void readAsync(HistoryReadRequest, ResourceContext, ReadCallback);
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
