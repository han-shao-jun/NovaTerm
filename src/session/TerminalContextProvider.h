/** @file TerminalContextProvider.h
 *  @brief 会话文本上下文提供器；按需采样 Core，不访问 Renderer 或原始字节流。
 */
#pragma once
#include "TerminalStateCache.h"
#include "core/terminal/TerminalCore.h"
#include <QPointer>
#include <QtGlobal>
#include <chrono>

namespace NovaTerm {
/** @brief 上下文读取是否允许退回 Parser 按需发布的不可变快照。
 *
 * 默认启用。仅在需要对比旧 try-read 行为时，把该环境变量置 0 显式退回；
 * 生产路径不再读它做取数分岔。集中在此避免 Provider、McpService、McpAccess
 * 三处各自解析同一环境变量。
 *
 * @note 判据只接受「未设置」或「可解析且非 0」。qEnvironmentVariableIntValue
 * 对无法解析的值返回 0，因此 `false` / `off` / 乱码都会静默退回旧路径；方向是
 * 保守的（退回更严格的纯 try-read），但与「置 0 才退回」的注释不完全一致。
 * @note 变量名只出现一次：同一表达式里写两遍时，任一处拼错都会因短路或
 * 解析失败而永久改变默认行为，且不产生任何编译或测试信号。
 */
[[nodiscard]] inline bool publishedContextSnapshotEnabled()
{
    static constexpr char EnvName[] = "NOVATERM_MCP_PUBLISHED_SNAPSHOT";
    // 一次读取同时判空与非 0，避免两次环境表查找，也避免名字重复。
    const QByteArray value = qgetenv(EnvName);
    return value.isEmpty() || value.toInt() != 0;
}
} // namespace NovaTerm

/** @note 在所属 Session 线程调用；返回值独立拥有数据，可交给 Agent。 */
class TerminalContextProvider final
{
public:
    struct Request {
        NovaTerm::u64 sinceRevision{0};
        std::size_t maxBytes{65536};
        std::size_t maxLines{256};
        bool viewport{true};
    };
    struct Context : NovaTerm::TerminalState {
        bool resetRequired{false};
        NovaTerm::u64 suppressedDuplicates{0};
    };
    /** @brief 不含客户端进度的不可变基础摘要；各消费者独立筛选 revision。 */
    struct Snapshot {
        NovaTerm::TerminalState state;
        std::vector<TerminalStateCache::Entry> entries;
        NovaTerm::u64 cacheFloor{0};
        NovaTerm::u64 resetRevision{0};
        NovaTerm::u64 suppressedDuplicates{0};
        std::chrono::system_clock::time_point capturedAt{};
    };
    explicit TerminalContextProvider(TerminalCore* core) : _core(core) {}
    void reset() { _cache.clear(); _state = {}; _historyId = 0; _resetRevision = 0; _initialized = false; _capturedAt = {}; _snapshot.reset(); }
    /** @brief 取当前有界摘要；先直接 try-read，模型忙时退到 Parser 发布的不可变快照。
     *
     * 顺序很重要：try-read 是非阻塞 try-lock，模型空闲时总能取到**最新**数据，
     * 所以它是主路径；只有在持续输出把模型锁占满、try-read 失败时，才用发布物
     * 兜底，从而消除 read starvation（不再直接返回 Busy）。发布物带硬性年龄上限，
     * 避免解析器停摆后把很久以前的快照当成当前数据。两条路径都不阻塞 GUI。 */
    [[nodiscard]] std::shared_ptr<const Snapshot> trySnapshot()
    {
        if (!_core)
            return {};
        auto next = _core->tryTerminalState(_historyId,
            TerminalStateCache::MaxBytes, TerminalStateCache::MaxLines);
        if (next) {
            if (!_initialized || next->revision != _state.revision)
                acceptState(std::move(*next));
            _capturedAt = std::chrono::system_clock::now();
        } else if (NovaTerm::publishedContextSnapshotEnabled()) {
            // 传自己的高水位，使发布物的 truncated/增量语义与直接捕获一致；
            // 固定传 0 会让发布物恒带 truncated，消费方每次都被迫全量重发。
            const auto published = _core->requestPublishedTerminalState(_historyId);
            if (!published)
                return {};
            // 年龄判据双侧拒绝：system_clock 被 NTP 回拨时年龄为负，只判上界会
            // 让任意旧发布物通过；负值同样按不可用处理。
            const auto age = std::chrono::system_clock::now()
                - published->capturedAt;
            if (age < std::chrono::seconds(0) || age > MaxPublishAge)
                return {};
            // 只接受前进的 revision。发布物是「请求时」的快照，可能落后于上一次
            // 成功的 try-read；接受更旧的 revision 会走 acceptState 的回退分支清空
            // 缓存，而 _historyId 不随之归零，缓存无法重建。
            if (_initialized && published->state.revision <= _state.revision) {
                // 没有更新可取：沿用既有快照，不伪造 capturedAt。
                return _snapshot ? _snapshot : std::shared_ptr<const Snapshot>{};
            }
            acceptState(published->state);
            _capturedAt = published->capturedAt;
        } else {
            return {};
        }
        if (!_snapshot) {
            auto snapshot = std::make_shared<Snapshot>();
            snapshot->state = _state;
            snapshot->entries.assign(_cache.entries().begin(), _cache.entries().end());
            snapshot->cacheFloor = _cache.floor();
            snapshot->resetRevision = _resetRevision;
            snapshot->suppressedDuplicates = _cache.duplicates();
            snapshot->capturedAt = _capturedAt;
            _snapshot = std::move(snapshot);
        }
        return _snapshot;
    }
    [[nodiscard]] Context context(const Request& request)
    {
        if (!_core)
            return {};
        if (!_initialized || _core->modelRevision() != _state.revision) {
            acceptState(_core->terminalState(_historyId,
                TerminalStateCache::MaxBytes, TerminalStateCache::MaxLines));
        }
        Context result;
        result.revision = _state.revision;
        result.cursor = _state.cursor;
        result.alternateScreen = _state.alternateScreen;
        result.truncated = _state.truncated;
        result.resetRequired = request.sinceRevision < _resetRevision
            || request.sinceRevision > result.revision || _state.truncated
            || (request.sinceRevision != 0 && request.sinceRevision <= _cache.floor());
        result.suppressedDuplicates = _cache.duplicates();
        std::size_t bytes = std::min(request.maxBytes, TerminalStateCache::MaxBytes);
        std::size_t lines = std::min(request.maxLines, TerminalStateCache::MaxLines);
        const auto copyText = [&](const std::string& text) {
            std::size_t count = std::min(bytes, text.size());
            if (count < text.size()) {
                while (count > 0 && (static_cast<unsigned char>(text[count]) & 0xc0) == 0x80)
                    --count;
                result.truncated = true;
            }
            bytes -= count;
            return text.substr(0, count);
        };
        result.title = copyText(_state.title);
        const auto copyLine = [&](const NovaTerm::TerminalStateLine& line,
                                  std::vector<NovaTerm::TerminalStateLine>& target) {
            if (lines == 0 || bytes == 0) { result.truncated = true; return; }
            target.push_back({line.id, copyText(line.text), line.complete});
            --lines;
        };
        if (request.viewport) {
            for (const auto& line : _state.viewport)
                copyLine(line, result.viewport);
        }
        if (!_state.alternateScreen) {
            for (const auto& entry : _cache.entries()) {
                if (entry.revision > request.sinceRevision || result.resetRequired)
                    copyLine({entry.revision, entry.text}, result.recentOutput);
            }
        }
        return result;
    }
private:
    /** @brief 可被沿用的发布物最大年龄。发布只在模型忙时使用，而模型忙意味着
     *  解析器仍在提交，发布物会按 250 ms 闸门持续刷新；该上限只是解析器停摆时
     *  的兜底闸门，宁可返回 Busy 也不把旧快照当当前数据。 */
    static constexpr std::chrono::seconds MaxPublishAge{2};

    void acceptState(NovaTerm::TerminalState next)
    {
        if (_initialized && (next.alternateScreen != _state.alternateScreen
                             || next.revision < _state.revision)) {
            _cache.clear();
            _resetRevision = next.revision;
        }
        if (!next.alternateScreen) {
            for (const auto& line : next.recentOutput) {
                if (line.id <= _historyId)
                    continue;
                _cache.append(next.revision, line.text);
                _historyId = std::max(_historyId, line.id);
            }
            // 光标所在行可能被 CR 改写，只在活动屏幕展示，不累积版本。
            for (const auto& line : next.viewport) {
                if (line.complete)
                    _cache.append(next.revision, line.text);
            }
        }
        next.recentOutput.clear();
        _state = std::move(next);
        _initialized = true;
        _snapshot.reset();
    }
    QPointer<TerminalCore> _core;
    TerminalStateCache _cache;
    NovaTerm::TerminalState _state;
    NovaTerm::u64 _historyId{0};
    NovaTerm::u64 _resetRevision{0};
    bool _initialized{false};
    std::chrono::system_clock::time_point _capturedAt{};
    std::shared_ptr<const Snapshot> _snapshot;
};
