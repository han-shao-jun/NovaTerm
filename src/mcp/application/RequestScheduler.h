/**
 * @file RequestScheduler.h
 * @brief 每 peer 的请求预算、排队与交付状态；不判断远端执行是否结束。
 *
 * 交付（是否排队、已投递、已取消）与执行生命周期分开：本类只跟踪请求
 * 何时可以开始执行，任务结果与资源回收由执行方负责。被取消的排队请求不会
 * 再被投递；已投递请求占用名额直到 `complete()`。
 */
#pragma once

#include <QString>

#include <cstddef>
#include <deque>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NovaTerm::Mcp {

struct RequestSchedulerLimits
{
    int maxActive{2};
    int maxQueued{4};
};

class RequestScheduler
{
public:
    struct Delivery
    {
        QString peer;
        QString request;
        std::function<void()> run;
    };

    enum class Admission
    {
        Accepted,
        QueueFull,
    };

    explicit RequestScheduler(RequestSchedulerLimits limits = {});

    [[nodiscard]] Admission enqueue(const QString& peer, const QString& request,
                                    std::function<void()> run);
    /** @brief 取出当前可执行的请求（每 peer 至多补齐到 maxActive）。 */
    [[nodiscard]] std::vector<Delivery> takeReady();
    [[nodiscard]] bool hasReady() const;

    void complete(const QString& peer, const QString& request);
    void cancel(const QString& peer, const QString& request);
    void removePeer(const QString& peer);

    [[nodiscard]] int activeCount(const QString& peer) const;
    [[nodiscard]] int queuedCount(const QString& peer) const;
    [[nodiscard]] std::size_t peerCount() const;
    [[nodiscard]] bool contains(const QString& peer, const QString& request) const;
    void clear();

private:
    struct Entry
    {
        QString request;
        std::function<void()> run;
        bool cancelled{false};
    };
    struct Peer
    {
        int active{0};
        std::deque<Entry> queued;
        std::unordered_set<std::string> activeRequests;
    };

    [[nodiscard]] Peer* findPeer(const QString& peer);
    [[nodiscard]] const Peer* findPeer(const QString& peer) const;

    RequestSchedulerLimits _limits;
    std::unordered_map<std::string, Peer> _peers;
};

} // namespace NovaTerm::Mcp
