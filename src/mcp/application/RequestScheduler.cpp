/** @file RequestScheduler.cpp @brief 每 peer 请求预算与交付队列实现。 */
#include "RequestScheduler.h"

#include <utility>

namespace NovaTerm::Mcp {

namespace {
std::string key(const QString& value) { return value.toStdString(); }
} // namespace

RequestScheduler::RequestScheduler(RequestSchedulerLimits limits) : _limits(limits) {}

RequestScheduler::Peer* RequestScheduler::findPeer(const QString& peer)
{
    const auto found = _peers.find(key(peer));
    return found == _peers.end() ? nullptr : &found->second;
}

const RequestScheduler::Peer* RequestScheduler::findPeer(const QString& peer) const
{
    const auto found = _peers.find(key(peer));
    return found == _peers.end() ? nullptr : &found->second;
}

RequestScheduler::Admission RequestScheduler::enqueue(const QString& peer,
    const QString& request, std::function<void()> run)
{
    auto& state = _peers[key(peer)];
    if (int(state.queued.size()) >= _limits.maxQueued)
        return Admission::QueueFull;
    state.queued.push_back(Entry{request, std::move(run), false});
    return Admission::Accepted;
}

std::vector<RequestScheduler::Delivery> RequestScheduler::takeReady()
{
    std::vector<Delivery> deliveries;
    for (auto& peer : _peers) {
        auto& state = peer.second;
        while (state.active < _limits.maxActive && !state.queued.empty()) {
            auto entry = std::move(state.queued.front());
            state.queued.pop_front();
            if (entry.cancelled)
                continue;
            ++state.active;
            state.activeRequests.insert(entry.request.toStdString());
            deliveries.push_back(Delivery{QString::fromStdString(peer.first),
                entry.request, std::move(entry.run)});
        }
    }
    return deliveries;
}

bool RequestScheduler::hasReady() const
{
    for (const auto& peer : _peers) {
        if (peer.second.active < _limits.maxActive && !peer.second.queued.empty())
            return true;
    }
    return false;
}

void RequestScheduler::complete(const QString& peer, const QString& request)
{
    auto* state = findPeer(peer);
    if (!state)
        return;
    if (state->activeRequests.erase(request.toStdString()) != 0) {
        if (state->active > 0)
            --state->active;
    }
    // 未投递就结束（取消/提前拒绝）的请求不再占用队列。
    for (auto it = state->queued.begin(); it != state->queued.end(); ++it) {
        if (it->request == request) {
            state->queued.erase(it);
            break;
        }
    }
}

void RequestScheduler::cancel(const QString& peer, const QString& request)
{
    auto* state = findPeer(peer);
    if (!state)
        return;
    if (state->activeRequests.find(request.toStdString()) != state->activeRequests.end())
        return; // 已投递：由执行方 complete() 结束；取消语义由任务自身承载。
    for (auto& entry : state->queued) {
        if (entry.request == request) {
            entry.cancelled = true;
            return;
        }
    }
}

void RequestScheduler::removePeer(const QString& peer)
{
    _peers.erase(key(peer));
}

int RequestScheduler::activeCount(const QString& peer) const
{
    const auto* state = findPeer(peer);
    return state ? state->active : 0;
}

int RequestScheduler::queuedCount(const QString& peer) const
{
    const auto* state = findPeer(peer);
    return state ? int(state->queued.size()) : 0;
}

std::size_t RequestScheduler::peerCount() const { return _peers.size(); }

bool RequestScheduler::contains(const QString& peer, const QString& request) const
{
    const auto* state = findPeer(peer);
    if (!state)
        return false;
    if (state->activeRequests.find(request.toStdString()) != state->activeRequests.end())
        return true;
    for (const auto& entry : state->queued)
        if (entry.request == request)
            return true;
    return false;
}

void RequestScheduler::clear() { _peers.clear(); }

} // namespace NovaTerm::Mcp
