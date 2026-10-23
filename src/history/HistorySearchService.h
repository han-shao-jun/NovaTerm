#pragma once
#include "HistoryReadService.h"
#include <QThreadPool>
#include <QTimer>
namespace NovaTerm::Recording {
class HistorySearchService final : public QObject {
public:
    struct Limits {
        size_t states{16}, principalStates{2};
        quint64 scanBytes{16*1024*1024};
        std::chrono::milliseconds pageTime{2000}, idleTtl{60000}, absoluteTtl{300000};
    };
    explicit HistorySearchService(HistoryReadService&, QObject* parent = nullptr);
    HistorySearchService(HistoryReadService&, Limits, QObject* parent = nullptr);
    ~HistorySearchService() override;
    void searchAsync(HistorySearchRequest, ResourceContext, std::function<void(HistorySearchPage)>, Cancellation = {});
    void sweep();
private:
    struct State;
    struct Mailbox;
    HistoryReadService& _reads;
    Limits _limits;
    QThreadPool _pool;
    QTimer _sweep;
    std::shared_ptr<Mailbox> _mailbox;
    std::vector<std::shared_ptr<State>> _states;
    void step(const std::shared_ptr<State>&);
    void compute(const std::shared_ptr<State>&);
    void finish(const std::shared_ptr<State>&, QString error = {});
};
}
