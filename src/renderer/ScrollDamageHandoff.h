/**
 * @file   ScrollDamageHandoff.h
 * @brief  活动屏幕上滚行数的两段交接：等到对应内容帧到达才允许旋转行映射。
 *
 * `TerminalCore::screenScrolled` 先于同一解析批次的 damage 区域送达 GUI
 * 线程，两者不是同一个信号。若 Renderer 一收到滚动行数就旋转 CPU 命令行
 * 与 GPU 行槽位环，那么在下一个内容帧到达之前插入的 Overlay-only 帧
 * （光标闪烁、选区变化）就会用**已旋转但尚无新内容**的映射去绘制，导致错行。
 *
 * 因此把滚动行数分两级暂存：
 *
 * ```text
 * screenScrolled(n)        -> queue(n)        累计到 queuedRows
 * frameRequested(含内容)   -> publish()       queuedRows 转入 pendingRows
 * 渲染开始                 -> takePending()   取出并清零，旋转行映射
 * ```
 *
 * `takePending()` 只消费已 publish 的行数，不会抹掉仍排在 queuedRows 里、
 * 属于后续帧的滚动。调用点见 TerminalRenderer.cpp 的 queue / publish /
 * takePending 三处；回归测试见 RendererP5Tests.cpp 的
 * `scrollDamageHandoffWaitsForContentFrame()`。
 *
 * @note 纯值状态，无锁；由 Renderer 在 _pendingFrameMutex 保护下访问。
 *       只暴露 takePending() 一个观察点 —— queuedRows()/pendingRows()
 *       没有任何生产调用方，观察两段状态改由测试用 takePending() 的返回值
 *       序列表达。
 */
#pragma once

#include <QtGlobal>

#include <limits>
#include <utility>

namespace NovaTerm {

// 滚动行数的两段暂存器。详见文件头说明。
class ScrollDamageHandoff final
{
public:
    /**
     * @brief 记录一次 screenScrolled 报告的上滚行数（尚未与内容帧对齐）。
     * @param rows 上滚行数；<= 0 时忽略。
     */
    void queue(int rows) noexcept
    {
        if (rows <= 0)
            return;
        _queuedRows = saturatedAdd(_queuedRows, rows);
    }

    /**
     * @brief 内容帧（Full Frame 或带 damage 区域的帧）到达时调用，
     *        把 queuedRows 全部转入 pendingRows。
     * @note Overlay-only 帧**不得**调用本函数，否则会在没有新内容的情况下
     *       放行行映射旋转。
     */
    void publish() noexcept
    {
        _pendingRows = saturatedAdd(
            _pendingRows, std::exchange(_queuedRows, 0));
    }

    /**
     * @brief 取出并清零已对齐的上滚行数，供本帧旋转行映射使用。
     * @return 本帧应旋转的行数；0 表示不旋转。
     */
    [[nodiscard]] int takePending() noexcept
    {
        return std::exchange(_pendingRows, 0);
    }

private:
    // 行数上限饱和而非回绕：溢出后夹到 INT_MAX，让调用方走"滚动行数 >= 行数"
    // 的整屏重建路径，而不是得到一个负数并旋转错误的槽位。
    static int saturatedAdd(int current, int added) noexcept
    {
        Q_ASSERT(current >= 0);
        Q_ASSERT(added >= 0);
        return added > std::numeric_limits<int>::max() - current
            ? std::numeric_limits<int>::max() : current + added;
    }

    int _queuedRows{0};  ///< 已报告但对应内容帧尚未到达的上滚行数
    int _pendingRows{0};  ///< 已与内容帧对齐、等待本帧消费的上滚行数
};

} // namespace NovaTerm
