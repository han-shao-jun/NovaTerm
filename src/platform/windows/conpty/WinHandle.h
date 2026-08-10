/**
 * @file   WinHandle.h
 * @brief  Windows HANDLE 的 RAII 包装。
 *
 * 对 CloseHandle 语义做异常安全封装：移动语义转移所有权，析构自动关闭。
 * INVALID_HANDLE_VALUE 被规范化为 nullptr，避免误用。CloseHandle 失败时
 * 回退到 OutputDebugStringW 报告，不抛异常（析构 noexcept）。
 */
#pragma once

#include <windows.h>

#include <utility>

namespace NovaTerm::Windows {

/**
 * @brief Windows HANDLE 的 RAII 包装（不可拷贝，可移动）。
 *
 * 析构时调用 CloseHandle 释放；移动后原对象置 nullptr。reset() 失败时
 * 通过 OutputDebugStringW 报告，保证析构 noexcept。
 */
class WinHandle final
{
public:
    WinHandle() noexcept = default;

    /**
     * @brief 接管一个原生句柄。
     * @param handle 待接管的 HANDLE（INVALID_HANDLE_VALUE 会被规范化为 nullptr）。
     */
    explicit WinHandle(HANDLE handle) noexcept : _handle(normalize(handle)) {}

    ~WinHandle() { reset(); }

    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;

    WinHandle(WinHandle&& other) noexcept
        : _handle(std::exchange(other._handle, nullptr))
    {
    }

    WinHandle& operator=(WinHandle&& other) noexcept
    {
        if (this != &other)
            reset(std::exchange(other._handle, nullptr));
        return *this;
    }

    /** @brief 获取原生句柄（不转移所有权）。 */
    [[nodiscard]] HANDLE get() const noexcept { return _handle; }

    /** @brief 是否持有一个有效句柄。 */
    [[nodiscard]] explicit operator bool() const noexcept { return _handle != nullptr; }

    /**
     * @brief 释放所有权并返回原生句柄。
     * @return 原生句柄；本对象此后置 nullptr。
     */
    [[nodiscard]] HANDLE release() noexcept
    {
        return std::exchange(_handle, nullptr);
    }

    /**
     * @brief 关闭当前句柄并接管新句柄。
     * @param handle 新句柄（nullptr 表示仅关闭当前句柄）。
     * @note CloseHandle 失败时通过 OutputDebugStringW 报告，不抛异常。
     */
    void reset(HANDLE handle = nullptr) noexcept
    {
        handle = normalize(handle);
        if (_handle && !::CloseHandle(_handle))
            ::OutputDebugStringW(L"NovaTerm: CloseHandle failed in WinHandle fallback\n");
        _handle = handle;
    }

private:
    /// 将 INVALID_HANDLE_VALUE 规范化为 nullptr，统一判空逻辑。
    static HANDLE normalize(HANDLE handle) noexcept
    {
        return handle == INVALID_HANDLE_VALUE ? nullptr : handle;
    }

    HANDLE _handle{nullptr};
};

} // namespace NovaTerm::Windows
