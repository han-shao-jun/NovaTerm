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
    // 实测（root@192.168.10.100，2 核 ARM：`grep -c '^cpu[0-9]' /proc/stat` = 2；
    // 用「重复执行 + /proc/stat 前后差」测得）每条远端命令约 15ms CPU，且与命令
    // 内容基本无关 —— 固定开销来自远端 shell 与通道处理。因此**批次数（=命令数）
    // 比间隔更关键**：静态批已合并为 3 批（概览+ip / cpuinfo+频率 / lspci）。
    //   • 首批 1.2s：等常驻通道与交互 shell 的启动开销过去，且保证连接成功
    //     2~3 秒后点开详情对话框已有概览；
    //   • 批间隔 1.0s：每条命令独占一个采样区间，避免叠加成一个高峰；
    //   • 完成冷却 250ms：慢批次结束后不立刻补发。
    // 上界：1200 + 2*1000 = 3200ms 全部就绪。下面常量被
    // tests/transport/SshTransportFailureCheck.cpp 按常量推导断言，改动需同步跑该目标。
    static constexpr int BatchCount = 3;
    static constexpr int InitialDelayMs = 1200;
    static constexpr int BatchSpacingMs = 1000;
    static constexpr int CompletionGapMs = 250;

private:
    int _batch{0};
    bool _inFlight{false};
    std::int64_t _due{InitialDelayMs};
    std::int64_t _started{0};
};

} // namespace NovaTerm::LinuxResource
