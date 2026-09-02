/*
 * conpty_handle_leak_repro.c —— ConPTY 句柄泄漏最小复现（不参与 CMake 构建）
 *
 * 结论：`CreatePseudoConsole` / `ClosePseudoConsole` 在本机 Windows 版本上不配平，
 * 每创建并关闭一个伪控制台泄漏约 1 个内核句柄。NovaTerm 的 ConPtySession 自身
 * 句柄管理是正确的——它创建的 8 个句柄（2 条管道的宿主端、进程、主线程、Job、
 * 以及 3 个工作线程句柄）全部有对应的关闭点。
 *
 * 这解释了 ConPtyTransportTests 中两处句柄断言的失败：
 *   - injectedStartupStagesRollBack
 *   - repeatedLifecycleReturnsResourcesToBaseline
 * 二者都要求 200/15 次完整生命周期后进程句柄数回到基线，而平台开销无法消除。
 * 线程数与子进程数断言均通过，说明泄漏只发生在伪控制台这一项。
 *
 * 本程序刻意不创建子进程、不起工作线程、单线程运行，以排除 NovaTerm 代码与
 * 并发计数噪声的干扰；关闭顺序与 ConPtySession::createStartupResources() 一致
 * （CreatePseudoConsole 之后立即释放交给 ConPTY 的子端）。
 *
 * 手工编译与运行（需 MSVC 环境）：
 *   vcvarsall.bat x64
 *   cl /nologo /O2 conpty_handle_leak_repro.c /link /OUT:repro.exe
 *   repro.exe
 *
 * 2026-09-02 在 Windows 10 Enterprise 19045 / MSVC 19.51 上的实测输出：
 *   iter   0 handles 344
 *   iter  50 handles 403
 *   iter 100 handles 453
 *   iter 150 handles 503
 *   baseline 343 -> end 552  =>  每次循环泄漏 1.04 个句柄
 *
 * 若某个 Windows 版本修好了这个问题，本程序的泄漏值应降到 0，届时上述两处
 * 断言即可恢复为硬性检查。
 */
#include <windows.h>
#include <stdio.h>

typedef HRESULT (WINAPI *CreatePseudoConsoleFn)(COORD, HANDLE, HANDLE, DWORD, void**);
typedef void    (WINAPI *ClosePseudoConsoleFn)(void*);

static DWORD handleCount(void)
{
    DWORD n = 0;
    return GetProcessHandleCount(GetCurrentProcess(), &n) ? n : 0;
}

/* 一轮完整的伪控制台创建/关闭，无子进程、无线程。 */
static int cycle(CreatePseudoConsoleFn create, ClosePseudoConsoleFn close_)
{
    HANDLE inputRead = NULL, inputWrite = NULL;
    HANDLE outputRead = NULL, outputWrite = NULL;
    void* pseudoConsole = NULL;
    COORD size = {80, 24};

    if (!CreatePipe(&inputRead, &inputWrite, NULL, 0))
        return 0;
    if (!CreatePipe(&outputRead, &outputWrite, NULL, 0)) {
        CloseHandle(inputRead);
        CloseHandle(inputWrite);
        return 0;
    }
    if (create(size, inputRead, outputWrite, 0, &pseudoConsole) != S_OK) {
        CloseHandle(inputRead);  CloseHandle(inputWrite);
        CloseHandle(outputRead); CloseHandle(outputWrite);
        return 0;
    }
    /* 与 ConPtySession 同序：ConPTY 已复制所需端，宿主立即释放原始子端。 */
    CloseHandle(inputRead);
    CloseHandle(outputWrite);
    close_(pseudoConsole);
    CloseHandle(inputWrite);
    CloseHandle(outputRead);
    return 1;
}

int main(void)
{
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    CreatePseudoConsoleFn create =
        (CreatePseudoConsoleFn)GetProcAddress(kernel32, "CreatePseudoConsole");
    ClosePseudoConsoleFn close_ =
        (ClosePseudoConsoleFn)GetProcAddress(kernel32, "ClosePseudoConsole");
    const int iterations = 200;
    DWORD baseline = 0, final = 0;
    int i = 0;

    if (!create || !close_) {
        printf("ConPTY API unavailable on this Windows version\n");
        return 2;
    }

    /* 预热：首次调用会加载并初始化 conhost 相关模块，其一次性开销不计入基线。 */
    for (i = 0; i < 3; ++i) {
        if (!cycle(create, close_)) {
            printf("warmup cycle failed\n");
            return 3;
        }
    }

    baseline = handleCount();
    for (i = 0; i < iterations; ++i) {
        if (!cycle(create, close_)) {
            printf("cycle %d failed\n", i);
            return 4;
        }
        if (i % 50 == 0)
            printf("  iter %3d handles %lu\n", i, (unsigned long)handleCount());
    }
    final = handleCount();

    printf("baseline %lu -> end %lu  =>  每次循环泄漏 %.2f 个句柄\n",
           (unsigned long)baseline, (unsigned long)final,
           (double)((long)final - (long)baseline) / iterations);
    return 0;
}
