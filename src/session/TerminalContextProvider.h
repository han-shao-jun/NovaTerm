/** @file TerminalContextProvider.h
 *  @brief 会话文本上下文提供器；按需采样 Core，不访问 Renderer 或原始字节流。
 */
#pragma once
#include "TerminalStateCache.h"
#include "core/terminal/TerminalCore.h"
#include <QPointer>

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
    explicit TerminalContextProvider(TerminalCore* core) : _core(core) {}
    void reset() { _cache.clear(); _state = {}; _historyId = 0; _resetRevision = 0; _initialized = false; }
    [[nodiscard]] Context context(const Request& request)
    {
        if (!_core)
            return {};
        if (!_initialized || _core->modelRevision() != _state.revision) {
            auto next = _core->terminalState(_historyId,
                TerminalStateCache::MaxBytes, TerminalStateCache::MaxLines);
            if (_initialized && (next.alternateScreen != _state.alternateScreen
                                 || next.revision < _state.revision)) {
                _cache.clear();
                _resetRevision = next.revision;
            }
            if (!next.alternateScreen) {
                for (const auto& line : next.recentOutput) {
                    _cache.append(next.revision, line.text);
                    _historyId = std::max(_historyId, line.id);
                }
                // 活动光标所在行可被 CR 覆盖，只保留在 viewport，永不累积版本。
                for (const auto& line : next.viewport) {
                    if (line.complete)
                        _cache.append(next.revision, line.text);
                }
            }
            _state = std::move(next);
            _initialized = true;
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
            target.push_back({line.id, copyText(line.text)});
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
    QPointer<TerminalCore> _core;
    TerminalStateCache _cache;
    NovaTerm::TerminalState _state;
    NovaTerm::u64 _historyId{0};
    NovaTerm::u64 _resetRevision{0};
    bool _initialized{false};
};
