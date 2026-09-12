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
    void finished(std::int64_t now) noexcept
    {
        _inFlight = false;
        ++_batch;
        // 起始间距限制密度，完成后的冷却避免慢批次结束后紧接着补发。
        _due = std::max(_started + BatchSpacingMs, now + CompletionGapMs);
    }

    // 采集时机分两段：常驻通道只喂面板（CPU/内存/交换/网络）与低频 df；
    // 系统信息对话框的详情按批分时铺开，避免握手后瞬间在远端堆起多条命令。
    //   • 首批 400ms：概览（os/kernel/host/arch/connection）最先就绪 —— 保证
    //     连接成功 2~3 秒后点开详情对话框就有内容；
    //   • 批间隔 800ms：最重的 lspci 排在最后一批，远端 CPU 不被短时抬高；
    //   • 完成冷却 250ms：慢批次结束后不立刻补发，避免叠加。
    // 上界：400 + 3*800 = 2800ms 全部就绪。下面三个常量被
    // tests/transport/SshTransportFailureCheck.cpp 按常量推导断言，改动需同步跑该目标。
    static constexpr int BatchCount = 4;
    static constexpr int InitialDelayMs = 400;
    static constexpr int BatchSpacingMs = 800;
    static constexpr int CompletionGapMs = 250;

private:
    int _batch{0};
    bool _inFlight{false};
    std::int64_t _due{InitialDelayMs};
    std::int64_t _started{0};
};

} // namespace NovaTerm::LinuxResource
