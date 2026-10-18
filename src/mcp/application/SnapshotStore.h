/**
 * @file SnapshotStore.h
 * @brief 每个 Session 共享的基础摘要缓存；合并窗口内的请求复用同一次采样。
 *
 * 纯模板，不依赖 Session 类型或线程设施；时钟由调用方注入单调毫秒源。
 * 缓存键是 Session 与会话 epoch：换绑后旧摘要不会被当成新连接的数据。
 */
#pragma once

#include <QHash>
#include <QString>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>

namespace NovaTerm::Mcp {

template <class Snapshot>
class SnapshotStore
{
public:
    using Clock = std::function<qint64()>;
    using Loader = std::function<std::shared_ptr<const Snapshot>()>;

    explicit SnapshotStore(Clock clock = {},
        std::chrono::milliseconds interval = std::chrono::milliseconds{250})
        : _clock(std::move(clock)), _intervalMs(interval.count())
    {
    }

    /**
     * @brief 取得基础摘要；窗口内复用，窗口外调用 loader。
     *
     * @param reused 输出参数：本次是否复用了合并窗内的旧摘要。
     * @return loader 失败时返回空指针，不缓存失败结果。
     */
    [[nodiscard]] std::shared_ptr<const Snapshot> acquire(const QString& session,
        const QString& epoch, const Loader& load, bool* reused = nullptr)
    {
        const qint64 now = _clock ? _clock() : 0;
        auto entry = _entries.find(session);
        if (entry != _entries.end() && entry->epoch == epoch && entry->snapshot
            && now - entry->sampledAt < _intervalMs) {
            if (reused)
                *reused = true;
            return entry->snapshot;
        }
        if (reused)
            *reused = false;
        auto snapshot = load ? load() : std::shared_ptr<const Snapshot>{};
        if (!snapshot)
            return {};
        _entries.insert(session, Entry{epoch, now, snapshot});
        return snapshot;
    }

    void remove(const QString& session) { _entries.remove(session); }
    void clear() { _entries.clear(); }
    [[nodiscard]] std::size_t size() const { return std::size_t(_entries.size()); }

private:
    struct Entry
    {
        QString epoch;
        qint64 sampledAt{0};
        std::shared_ptr<const Snapshot> snapshot;
    };

    QHash<QString, Entry> _entries;
    Clock _clock;
    qint64 _intervalMs{250};
};

} // namespace NovaTerm::Mcp
