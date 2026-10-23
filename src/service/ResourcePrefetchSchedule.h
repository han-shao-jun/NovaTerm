/**
 * @file ResourcePrefetchSchedule.h
 * @brief 详情预取的分批时序，使用调用方提供的单调时钟。
 */
#pragma once

#include <algorithm>
#include <cstdint>

namespace NovaTerm::LinuxResource {

class PrefetchSchedule
{
public:
    [[nodiscard]] int batch() const noexcept { return _batch; }
    [[nodiscard]] bool complete() const noexcept { return _batch == BatchCount; }
    [[nodiscard]] bool inFlight() const noexcept { return _inFlight; }
    [[nodiscard]] std::int64_t delay(std::int64_t now) const noexcept
    {
        return std::max<std::int64_t>(0, _due - now);
    }
    [[nodiscard]] bool ready(std::int64_t now) const noexcept
    {
        return !complete() && !_inFlight && delay(now) == 0;
    }
    void started(std::int64_t now) noexcept
    {
        _inFlight = true;
        _started = now;
    }
    void finished(std::int64_t now, bool success = true) noexcept
    {
        _inFlight = false;
        ++_batch;
        // 详情保持原分批节奏；慢批次完成后仍留冷却，不集中补发。
        _due = std::max(_started + BatchSpacingMs,
                        now + (success ? CompletionGapMs : FailureBackoffMs));
    }

    // 基础指标先显示，详情首批延后、批次错峰，避免集中增加远端负载。
    static constexpr int BatchCount = 3;
    static constexpr int InitialDelayMs = 1200;
    static constexpr int BatchSpacingMs = 1000;
    static constexpr int CompletionGapMs = 250;
    static constexpr int FailureBackoffMs = 1000;

private:
    int _batch{0};
    bool _inFlight{false};
    std::int64_t _due{InitialDelayMs};
    std::int64_t _started{0};
};

} // namespace NovaTerm::LinuxResource
