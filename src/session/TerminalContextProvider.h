/** @file TerminalContextProvider.h
 *  @brief 会话文本上下文提供器；按需采样 Core，不访问 Renderer 或原始字节流。
 */
#pragma once
#include "TerminalStateCache.h"
#include "core/terminal/TerminalCore.h"
#include <QPointer>
#include <QtGlobal>
#include <chrono>

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
    /** @brief 模型忙时立即返回空；不使用任何客户端的 sinceRevision 消费共享状态。 */
    [[nodiscard]] std::shared_ptr<const Snapshot> trySnapshot()
    {
        if (!_core)
            return {};
        const bool usePublished = qEnvironmentVariableIntValue(
            "NOVATERM_MCP_PUBLISHED_SNAPSHOT") > 0;
        if (usePublished) {
            const auto published = _core->requestPublishedTerminalState();
            if (!published)
                return {};
            if (!_initialized || published->state.revision != _state.revision)
                acceptState(published->state);
            _capturedAt = published->capturedAt;
        } else {
            // 兼容路径：模型忙时立即失败，不延长 GUI 锁等待。
            auto next = _core->tryTerminalState(_historyId,
                TerminalStateCache::MaxBytes, TerminalStateCache::MaxLines);
            if (!next)
                return {};
            if (!_initialized || next->revision != _state.revision)
                acceptState(std::move(*next));
            _capturedAt = std::chrono::system_clock::now();
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
