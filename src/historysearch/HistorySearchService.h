#pragma once
#include "HistoryAuthorizationStore.h"
#include <QObject>
#include <memory>
namespace NovaTerm::Recording {
class HistorySearchService final : public QObject {
public:
    using SearchCallback = std::function<void(HistorySearchPage)>;
    explicit HistorySearchService(HistoryReadPort, HistoryAuthorizationStore&, QObject* parent = nullptr,
                                  HistorySearchLimits = {});
    ~HistorySearchService() override;
    HistorySearchService(const HistorySearchService&) = delete;
    HistorySearchService& operator=(const HistorySearchService&) = delete;
    HistorySearchService(HistorySearchService&&) = delete;
    HistorySearchService& operator=(HistorySearchService&&) = delete;
    void searchAsync(HistorySearchRequest, ResourceContext, SearchCallback);
    bool cancel(const QString& cursor, const ResourceContext&);
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
