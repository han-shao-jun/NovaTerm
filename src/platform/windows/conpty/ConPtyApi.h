#pragma once

#include <windows.h>

#include <QString>

#include <utility>

namespace NovaTerm::Windows {

using CreatePseudoConsoleFunction = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
using ResizePseudoConsoleFunction = HRESULT(WINAPI*)(HPCON, COORD);
using ClosePseudoConsoleFunction = void(WINAPI*)(HPCON);

class ConPtyApi final
{
public:
    static bool resolve(QString* error = nullptr);
    static CreatePseudoConsoleFunction create();
    static ResizePseudoConsoleFunction resize();
    static ClosePseudoConsoleFunction close();
};

class PseudoConsoleHandle final
{
public:
    PseudoConsoleHandle() noexcept = default;
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

    [[nodiscard]] HPCON get() const noexcept { return _handle; }
    [[nodiscard]] explicit operator bool() const noexcept { return _handle != nullptr; }
    [[nodiscard]] HPCON release() noexcept { return std::exchange(_handle, nullptr); }
    void reset(HPCON handle = nullptr) noexcept;

private:
    HPCON _handle{nullptr};
};

[[nodiscard]] QString windowsErrorMessage(unsigned long error);
[[nodiscard]] QString hresultMessage(HRESULT result);

} // namespace NovaTerm::Windows
