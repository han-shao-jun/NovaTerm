#ifdef _WIN32
#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

bool writeAll(const char* data, std::size_t size)
{
    static HANDLE output = CreateFileW(L"CONOUT$", GENERIC_WRITE | GENERIC_READ,
                                       FILE_SHARE_WRITE | FILE_SHARE_READ,
                                       nullptr, OPEN_EXISTING, 0, nullptr);
    if (output == INVALID_HANDLE_VALUE)
        return false;
    std::size_t offset = 0;
    while (offset < size) {
        DWORD written = 0;
        const DWORD request = static_cast<DWORD>((std::min)(
            size - offset, std::size_t(64 * 1024)));
        if (!WriteFile(output, data + offset, request, &written, nullptr)
            || written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

bool writeAll(const std::string& value)
{
    return writeAll(value.data(), value.size());
}

std::string utf8(const wchar_t* value)
{
    const int length = static_cast<int>(wcslen(value));
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value, length,
                                          nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, length, result.data(), bytes,
                        nullptr, nullptr);
    return result;
}

std::uint64_t number(const wchar_t* value)
{
    return _wcstoui64(value, nullptr, 10);
}

std::uint64_t fnv1a(const char* data, std::size_t size,
                    std::uint64_t hash = 1469598103934665603ULL)
{
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= static_cast<unsigned char>(data[index]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

int runProbe()
{
    std::vector<wchar_t> directory(32768);
    const DWORD directoryLength = GetCurrentDirectoryW(
        static_cast<DWORD>(directory.size()), directory.data());
    if (directoryLength == 0 || directoryLength >= directory.size())
        return 20;
    std::vector<wchar_t> environment(32768);
    const DWORD environmentLength = GetEnvironmentVariableW(
        L"NOVATERM_CONPTY_PROBE", environment.data(),
        static_cast<DWORD>(environment.size()));
    if (environmentLength == 0 || environmentLength >= environment.size())
        return 21;
    return writeAll("CWD=" + utf8(directory.data()) + "\nENV="
                    + utf8(environment.data()) + "\n") ? 0 : 22;
}

int runArgv(int argc, wchar_t** argv)
{
    for (int index = 2; index < argc; ++index) {
        const std::string value = utf8(argv[index]);
        if (!writeAll(std::to_string(value.size()) + ":" + value + "\n"))
            return 30;
    }
    return 0;
}

int runDuplex(std::uint64_t inputBytes, std::uint64_t outputBytes)
{
    HANDLE input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (input == INVALID_HANDLE_VALUE)
        return 39;
    DWORD mode = 0;
    if (!GetConsoleMode(input, &mode))
        return 40;
    mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
    mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    if (!SetConsoleMode(input, mode))
        return 41;

    std::thread outputThread([outputBytes] {
        std::vector<char> block(64 * 1024, 'O');
        std::uint64_t remaining = outputBytes;
        while (remaining > 0) {
            const std::size_t bytes = static_cast<std::size_t>((std::min)(
                remaining, static_cast<std::uint64_t>(block.size())));
            if (!writeAll(block.data(), bytes))
                return;
            remaining -= bytes;
        }
    });

    std::vector<char> buffer(64 * 1024);
    std::uint64_t received = 0;
    std::uint64_t hash = 1469598103934665603ULL;
    while (received < inputBytes) {
        DWORD bytesRead = 0;
        const DWORD request = static_cast<DWORD>((std::min)(
            inputBytes - received,
            static_cast<std::uint64_t>(buffer.size())));
        if (!ReadFile(input, buffer.data(), request, &bytesRead, nullptr)
            || bytesRead == 0) {
            outputThread.join();
            return 42;
        }
        hash = fnv1a(buffer.data(), bytesRead, hash);
        received += bytesRead;
    }
    outputThread.join();
    const std::string marker = "\nREAD=" + std::to_string(received)
        + ";HASH=" + std::to_string(hash) + "\n";
    return writeAll(marker) ? 0 : 43;
}

int runSizeProbe()
{
    Sleep(500);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    HANDLE output = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (output == INVALID_HANDLE_VALUE
        || !GetConsoleScreenBufferInfo(output, &info))
        return 50;
    const int columns = info.srWindow.Right - info.srWindow.Left + 1;
    const int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    return writeAll("SIZE=" + std::to_string(columns) + "x"
                    + std::to_string(rows) + "\n") ? 0 : 51;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
        return 2;
    const std::wstring mode = argv[1];
    if (mode == L"probe")
        return runProbe();
    if (mode == L"argv")
        return runArgv(argc, argv);
    if (mode == L"exit" && argc >= 3)
        return static_cast<int>(number(argv[2]));
    if (mode == L"hold") {
        Sleep(60000);
        return 0;
    }
    if (mode == L"crash") {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE,
                       0, nullptr);
        return 4;
    }
    if (mode == L"duplex" && argc >= 4)
        return runDuplex(number(argv[2]), number(argv[3]));
    if (mode == L"size")
        return runSizeProbe();
    return 3;
}
#else
int main() { return 0; }
#endif
