/**
 * @file   ConPtyApi.h
 * @brief  Windows ConPTY API 动态解析与句柄包装。
 *
 * 通过运行时 GetProcAddress 解析 CreatePseudoConsole/ResizePseudoConsole/
 * ClosePseudoConsole，支持低版本 Windows 回退。PseudoConsoleHandle 对 HPCON
 * 做 RAII 封装。windowsErrorMessage/hresultMessage 提供本地化错误描述。
 */
#pragma once

#include <windows.h>

#include <QString>

#include <utility>

namespace NovaTerm::Windows {

/// CreatePseudoConsole 函数指针类型
using CreatePseudoConsoleFunction = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
/// ResizePseudoConsole 函数指针类型
using ResizePseudoConsoleFunction = HRESULT(WINAPI*)(HPCON, COORD);
/// ClosePseudoConsole 函数指针类型
using ClosePseudoConsoleFunction = void(WINAPI*)(HPCON);

/**
 * @brief ConPTY API 动态解析器（线程安全单例式）。
 *
 * resolve() 通过 GetModuleHandleW + GetProcAddress 查找 kernel32 中的
 * ConPTY 函数，原子发布完整解析集（部分查找不污染全局缓存）。所有
 * getter 加锁返回已解析的函数指针。
 */
class ConPtyApi final
{
public:
    /**
     * @brief 解析 ConPTY API 函数集。
     * @param error 错误信息输出（可选）。
     * @return true 表示已成功解析或之前已解析；false 表示平台不支持。
     */
    static bool resolve(QString* error = nullptr);

    /** @brief 获取 CreatePseudoConsole 函数指针（未解析时为 nullptr）。 */
    static CreatePseudoConsoleFunction create();

    /** @brief 获取 ResizePseudoConsole 函数指针（未解析时为 nullptr）。 */
    static ResizePseudoConsoleFunction resize();

    /** @brief 获取 ClosePseudoConsole 函数指针（未解析时为 nullptr）。 */
    static ClosePseudoConsoleFunction close();
};

/**
 * @brief HPCON（伪控制台句柄）的 RAII 包装（不可拷贝，可移动）。
 *
 * reset() 调用 ClosePseudoConsole 释放；移动后原对象置 nullptr。
 * 与 WinHandle 不同，PseudoConsoleHandle 的 reset 实现位于 .cpp
 * （依赖 ConPtyApi::close()）。
 */
class PseudoConsoleHandle final
{
public:
    PseudoConsoleHandle() noexcept = default;

    /**
     * @brief 接管一个伪控制台句柄。
     * @param handle 待接管的 HPCON。
     */
    explicit PseudoConsoleHandle(HPCON handle) noexcept : _handle(handle) {}

    ~PseudoConsoleHandle() { reset(); }

    PseudoConsoleHandle(const PseudoConsoleHandle&) = delete;
    PseudoConsoleHandle& operator=(const PseudoConsoleHandle&) = delete;

    PseudoConsoleHandle(PseudoConsoleHandle&& other) noexcept
        : _handle(std::exchange(other._handle, nullptr))
    {
    }

    PseudoConsoleHandle& operator=(PseudoConsoleHandle&& other) noexcept
    {
        if (this != &other) {
            reset();
            _handle = std::exchange(other._handle, nullptr);
        }
        return *this;
    }

    /** @brief 获取原生句柄（不转移所有权）。 */
    [[nodiscard]] HPCON get() const noexcept { return _handle; }

    /** @brief 是否持有一个有效句柄。 */
    [[nodiscard]] explicit operator bool() const noexcept { return _handle != nullptr; }

    /**
     * @brief 释放所有权并返回原生句柄。
     * @return 原生句柄；本对象此后置 nullptr。
     */
    [[nodiscard]] HPCON release() noexcept { return std::exchange(_handle, nullptr); }

    /**
     * @brief 关闭当前句柄并接管新句柄。
     * @param handle 新句柄（nullptr 表示仅关闭当前句柄）。
     * @note 关闭通过 ConPtyApi::close() 解析的 ClosePseudoConsole 完成。
     */
    void reset(HPCON handle = nullptr) noexcept;

private:
    HPCON _handle{nullptr};
};

/**
 * @brief 将 Windows 错误码转为本地化描述。
 * @param error 错误码（通常来自 GetLastError）。
 * @return 系统错误描述；无对应文本时返回 "Windows error <code>"。
 */
[[nodiscard]] QString windowsErrorMessage(unsigned long error);

/**
 * @brief 将 HRESULT 转为本地化描述。
 * @param result HRESULT 值。
 * @return 等同 windowsErrorMessage（按无符号转译）。
 */
[[nodiscard]] QString hresultMessage(HRESULT result);

} // namespace NovaTerm::Windows
