/**
 * @file TaskPhases.h
 * @brief 脚本/命令任务阶段与单向推进规则。
 */
#pragma once

namespace NovaTerm::Mcp {

enum class TaskPhase
{
    Preparing,
    Reserved,
    Submitted,
    Observing,
    Final,
};

/** @brief 阶段只能单向前进；Final 可由任意非终态到达。 */
[[nodiscard]] inline bool taskPhaseCanAdvance(TaskPhase from, TaskPhase to)
{
    if (from == TaskPhase::Final)
        return false;
    if (to == TaskPhase::Final)
        return true;
    return int(to) == int(from) + 1;
}

} // namespace NovaTerm::Mcp
