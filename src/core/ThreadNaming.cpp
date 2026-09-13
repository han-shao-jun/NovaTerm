/**
 * @file   ThreadNaming.cpp
 * @brief  ThreadNaming.h 的平台实现。
 */
#include "core/ThreadNaming.h"

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__APPLE__) || defined(__linux__)
#  include <pthread.h>
#endif

namespace NovaTerm {
namespace {

#if defined(_WIN32)

// SetThreadDescription（Win10 1607+）在不同 Windows SDK 里的声明条件不一致
// （部分 SDK 要求 _WIN32_WINNT >= 0x0A00），故在运行时解析入口：任何 SDK 都能
// 编译，1607 以前的系统上退化为空实现。
using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);

SetThreadDescriptionFn resolveSetThreadDescription() noexcept
{
    const HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    if (!kernel32)
        return nullptr;
    // GetProcAddress 返回 FARPROC，转成目标函数指针是它的标准用法；MSVC 在
    // /W4 下会为此报 C4191，这里就地按标准用法关掉。
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4191)
#endif
    return reinterpret_cast<SetThreadDescriptionFn>(
        ::GetProcAddress(kernel32, "SetThreadDescription"));
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif
}

void setCurrentThreadNameImpl(const char* name) noexcept
{
    static const SetThreadDescriptionFn setDescription =
        resolveSetThreadDescription();
    if (!setDescription)
        return;
    // 名字约定为 ASCII，逐字符展开成 UTF-16 即可，避免 MultiByteToWideChar
    // 带来的代码页依赖与额外分配。
    wchar_t wide[MaxThreadNameBytes + 1] = {};
    for (std::size_t index = 0;
         index < MaxThreadNameBytes && name[index] != '\0'; ++index) {
        wide[index] =
            static_cast<wchar_t>(static_cast<unsigned char>(name[index]));
    }
    (void)setDescription(::GetCurrentThread(), wide);
}

#elif defined(__APPLE__) || defined(__linux__)

void setCurrentThreadNameImpl(const char* name) noexcept
{
    char truncated[MaxThreadNameBytes + 1] = {};
    for (std::size_t index = 0;
         index < MaxThreadNameBytes && name[index] != '\0'; ++index)
        truncated[index] = name[index];
#if defined(__APPLE__)
    // macOS 的 pthread_setname_np 不接受线程句柄，只能命名当前线程。
    (void)::pthread_setname_np(truncated);
#else
    (void)::pthread_setname_np(::pthread_self(), truncated);
#endif
}

#else

void setCurrentThreadNameImpl(const char* name) noexcept
{
    (void)name; // 该平台没有可用的线程命名接口，保持空实现。
}

#endif

} // namespace

void setCurrentThreadName(const char* name) noexcept
{
    if (!name || name[0] == '\0')
        return;
    setCurrentThreadNameImpl(name);
}

} // namespace NovaTerm
