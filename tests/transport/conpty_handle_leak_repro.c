/*
 * Standalone ConPTY handle investigation (not registered in CTest).
 * No Qt, application child process or application worker threads.
 * CreatePseudoConsole itself starts conhost.exe.
 *
 * MSVC developer prompt:
 *   cl /nologo /W4 /O2 conpty_handle_leak_repro.c /Fe:repro.exe
 *   repro.exe --inspect --settle-ms 5000
 *   repro.exe --input-first --settle-ms 5000
 *   repro.exe --pipes-only
 *
 * --inspect uses diagnostic-only NT handle snapshots. It never closes handles
 * owned by the API or dereferences the opaque HPCON implementation.
 * Exit: 0 = no net growth, 1 = growth, 2 = bad options, 3 = API/query failure.
 * See docs/architecture/ConPTY_Handle_Investigation_2026-10-19.md.
 */
#include <windows.h>
#include <winternl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef HRESULT (WINAPI *CreatePseudoConsoleFn)(COORD, HANDLE, HANDLE, DWORD, void**);
typedef void (WINAPI *ClosePseudoConsoleFn)(void*);
typedef NTSTATUS (NTAPI *QueryProcessFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef NTSTATUS (NTAPI *QueryObjectFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);

/* ProcessHandleInformation (51), available on Windows versions with ConPTY. */
typedef struct HandleEntry {
    HANDLE handle;
    ULONG_PTR handleCount, pointerCount;
    ULONG access, type, attributes, reserved;
} HandleEntry;
typedef struct HandleSnapshot {
    ULONG_PTR count, reserved;
    HandleEntry entries[1];
} HandleSnapshot;
static QueryProcessFn queryProcess;
static QueryObjectFn queryObject;

static DWORD handleCount(void)
{
    DWORD count = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &count)) {
        fprintf(stderr, "GetProcessHandleCount failed: %lu\n", GetLastError());
        exit(3);
    }
    return count;
}

static HandleSnapshot* snapshot(void)
{
    const ULONG capacity = 1024 * 1024;
    ULONG returned = 0;
    HandleSnapshot* result = (HandleSnapshot*)malloc(capacity);
    NTSTATUS status;
    if (!result) exit(3);
    status = queryProcess(GetCurrentProcess(), 51, result, capacity, &returned);
    if (status < 0 || returned > capacity
        || result->count > (capacity - offsetof(HandleSnapshot, entries)) / sizeof(HandleEntry)) {
        fprintf(stderr, "ProcessHandleInformation failed: 0x%08lx\n", (ULONG)status);
        free(result);
        exit(3);
    }
    return result;
}

static int contains(const HandleSnapshot* snapshot_, HANDLE handle)
{
    ULONG_PTR i;
    for (i = 0; i < snapshot_->count; ++i)
        if (snapshot_->entries[i].handle == handle) return 1;
    return 0;
}

static void describeProcesses(const char* phase, const HandleSnapshot* before,
                              const HandleSnapshot* after)
{
    ULONG_PTR i;
    for (i = 0; i < after->count; ++i) {
        const HandleEntry* entry = &after->entries[i];
        union { ULONG_PTR alignment; unsigned char bytes[4096]; } buffer;
        UNICODE_STRING* type = (UNICODE_STRING*)buffer.bytes;
        ULONG returned = 0;
        NTSTATUS status;
        if (contains(before, entry->handle)) continue;
        status = queryObject(entry->handle, 2, buffer.bytes, sizeof(buffer.bytes), &returned);
        if (status < 0) {
            fprintf(stderr, "ObjectTypeInformation failed: 0x%08lx\n", (ULONG)status);
            exit(3);
        }
        if (type->Length == 7 * sizeof(wchar_t) && wcsncmp(type->Buffer, L"Process", 7) == 0) {
            wchar_t path[32768];
            DWORD length = ARRAYSIZE(path), code = 0;
            const BOOL queried = GetExitCodeProcess(entry->handle, &code);
            printf("%s handle=%p type=Process access=0x%lx pid=%lu exitKnown=%d exit=%lu wait=%lu",
                   phase, entry->handle, entry->access, GetProcessId(entry->handle),
                   queried, code, WaitForSingleObject(entry->handle, 0));
            if (QueryFullProcessImageNameW(entry->handle, 0, path, &length))
                printf(" image=%ls", path);
            printf("\n");
        }
    }
}

static int closeHandle(HANDLE* handle)
{
    if (*handle && !CloseHandle(*handle)) {
        fprintf(stderr, "CloseHandle failed: %lu\n", GetLastError());
        return 0;
    }
    *handle = NULL;
    return 1;
}

static int cycle(CreatePseudoConsoleFn create, ClosePseudoConsoleFn close_,
                 int pipesOnly, int inputFirst, int inspect)
{
    HANDLE inputRead = NULL, inputWrite = NULL, outputRead = NULL, outputWrite = NULL;
    void* pseudoConsole = NULL;
    const COORD size = {80, 24};
    HandleSnapshot *before = NULL, *created = NULL, *closed = NULL;
    int ok = 0;
    if (!CreatePipe(&inputRead, &inputWrite, NULL, 0)
        || !CreatePipe(&outputRead, &outputWrite, NULL, 0)) {
        fprintf(stderr, "CreatePipe failed: %lu\n", GetLastError());
        goto cleanup;
    }
    if (inspect) before = snapshot();
    if (!pipesOnly) {
        const HRESULT result = create(size, inputRead, outputWrite, 0, &pseudoConsole);
        if (FAILED(result)) {
            fprintf(stderr, "CreatePseudoConsole failed: 0x%08lx\n", (ULONG)result);
            goto cleanup;
        }
    }
    if (inspect) {
        created = snapshot();
        describeProcesses("created", before, created);
    }
    ok = 1;
cleanup:
    /* ConPTY duplicates its pipe ends; release the application's originals. */
    ok = closeHandle(&inputRead) && ok;
    ok = closeHandle(&outputWrite) && ok;
    if (inputFirst) ok = closeHandle(&inputWrite) && ok;
    if (pseudoConsole) close_(pseudoConsole);
    ok = closeHandle(&inputWrite) && ok;
    ok = closeHandle(&outputRead) && ok;
    if (inspect && before && created) {
        closed = snapshot();
        describeProcesses("remaining", before, closed);
    }
    free(closed);
    free(created);
    free(before);
    return ok;
}

static int number(const char* value, int maximum)
{
    char* end = NULL;
    long parsed = strtol(value, &end, 10);
    if (!*value || *end || parsed < 0 || parsed > maximum) return -1;
    return (int)parsed;
}

int main(int argc, char** argv)
{
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    CreatePseudoConsoleFn create = (CreatePseudoConsoleFn)GetProcAddress(kernel32, "CreatePseudoConsole");
    ClosePseudoConsoleFn close_ = (ClosePseudoConsoleFn)GetProcAddress(kernel32, "ClosePseudoConsole");
    int iterations = 200, settleMs = 0, inspect = 0, pipesOnly = 0, inputFirst = 0, i;
    DWORD baseline, final, settled;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--inspect") == 0) inspect = 1;
        else if (strcmp(argv[i], "--pipes-only") == 0) pipesOnly = 1;
        else if (strcmp(argv[i], "--input-first") == 0) inputFirst = 1;
        else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) iterations = number(argv[++i], 10000);
        else if (strcmp(argv[i], "--settle-ms") == 0 && i + 1 < argc) settleMs = number(argv[++i], 60000);
        else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
            return 2;
        }
    }
    if (iterations <= 0 || settleMs < 0) return 2;
    if (!pipesOnly && (!create || !close_)) {
        fprintf(stderr, "ConPTY API unavailable\n");
        return 3;
    }
    if (inspect) {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        queryProcess = (QueryProcessFn)GetProcAddress(ntdll, "NtQueryInformationProcess");
        queryObject = (QueryObjectFn)GetProcAddress(ntdll, "NtQueryObject");
        if (!queryProcess || !queryObject) return 3;
    }
    printf("mode=%s order=%s iterations=%d settleMs=%d\n",
           pipesOnly ? "pipes-only" : "conpty", inputFirst ? "input-first" : "console-first",
           iterations, settleMs);
    for (i = 0; i < 3; ++i)
        if (!cycle(create, close_, pipesOnly, inputFirst, 0)) return 3;
    baseline = handleCount();
    for (i = 0; i < iterations; ++i)
        /* Inspect only the first measured cycle; growth covers all cycles. */
        if (!cycle(create, close_, pipesOnly, inputFirst, inspect && i == 0)) return 3;
    final = handleCount();
    Sleep((DWORD)settleMs);
    settled = handleCount();
    printf("baseline=%lu final=%lu settled=%lu delta=%ld perCycle=%.3f\n",
           baseline, final, settled, (long)settled - (long)baseline,
           ((double)settled - baseline) / iterations);
    return settled > baseline ? 1 : 0;
}
