#pragma once

#include <windows.h>

#include <utility>

namespace NovaTerm::Windows {

class WinHandle final
{
public:
    WinHandle() noexcept = default;
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

    [[nodiscard]] HANDLE get() const noexcept { return _handle; }
    [[nodiscard]] explicit operator bool() const noexcept { return _handle != nullptr; }

    [[nodiscard]] HANDLE release() noexcept
    {
        return std::exchange(_handle, nullptr);
    }

    void reset(HANDLE handle = nullptr) noexcept
    {
        handle = normalize(handle);
        if (_handle && !::CloseHandle(_handle))
            ::OutputDebugStringW(L"NovaTerm: CloseHandle failed in WinHandle fallback\n");
        _handle = handle;
    }

private:
    static HANDLE normalize(HANDLE handle) noexcept
    {
        return handle == INVALID_HANDLE_VALUE ? nullptr : handle;
    }

    HANDLE _handle{nullptr};
};

} // namespace NovaTerm::Windows
