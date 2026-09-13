/**
 * @file   ThreadNaming.h
 * @brief  为工作线程设置 OS 级线程名，便于调试器与性能分析器识别。
 *
 * 为什么需要它：
 * - std::thread 没有可移植的命名入口，未命名的 worker 在 `ps -L`、
 *   `top -H`、VS 调试器的"线程"窗口里只能看到进程名，排查卡死/死锁时
 *   无法区分是哪一个职责的线程；
 * - QThread::setObjectName() 会把名字写进内核线程名，但只在 Linux/Unix
 *   上成立：Qt 6.8 的 QThread 文档写明 "Note that this is currently not
 *   available with release builds on Windows"，即 Windows release 构建下
 *   QThread 的名字对 OS 与调试器不可见。
 *
 * 约定：**在每个线程入口函数的第一行**调用 setCurrentThreadName()，
 * std::thread 与 QThread 一律如此；QThread 仍照常设置 objectName 供 Qt 层
 * 调试使用，两处名字保持一致。命名规则取 "nvterm-<模块>"，模块内部的
 * 辅助线程取 "<模块>-<角色>"，例如 nvterm-search、conpty-reader。
 *
 * 平台映射：
 * - Windows：SetThreadDescription（Win10 1607+，release 构建同样生效，
 *   VS 调试器 / WinDbg / Process Explorer 可见）
 * - Linux：pthread_setname_np（等价 prctl(PR_SET_NAME)，即
 *   /proc/<pid>/task/<tid>/comm，`ps -L`、`top -H` 可见）
 * - macOS：pthread_setname_np（该平台上只能命名当前线程）
 * - 其他平台：空实现
 */
#pragma once

#include <cstddef>

namespace NovaTerm {

/// 线程名长度上限：Linux 的 TASK_COMM_LEN(16) 减去结尾 NUL。
/// glibc 的 pthread_setname_np 对超长名字返回 ERANGE、内核名不变，因此
/// 实现统一按此长度截断，保证三个平台看到相同的名字。
inline constexpr std::size_t MaxThreadNameBytes = 15;

/**
 * @brief 设置当前线程的 OS 级线程名。
 *
 * @param name 以 NUL 结尾的 ASCII 线程名（如 "nvterm-parser"）。超过
 *             MaxThreadNameBytes 的部分会被截断；传 nullptr 或空串
 *             为空操作。
 * @note 必须由目标线程自己调用（macOS 只能命名当前线程）。可重复调用，
 *       以最后一次为准。
 * @note 不分配内存、不抛异常，可在真实线程入口第一时间调用。
 */
void setCurrentThreadName(const char* name) noexcept;

} // namespace NovaTerm
