// Minimal sys/mman.h stub for MSVC
// Provides mmap/munmap types and constants for compilation only.
// These are NOT functional on Windows — QTermWidget's History/BlockArray
// uses QTemporaryFile fallback when mmap is unavailable.

#pragma once

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// Protection flags
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

// Map flags
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON       MAP_ANONYMOUS

// mmap return value on failure
#define MAP_FAILED ((void*)-1)

// madvise flags
#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4

// Stub function declarations
static __inline void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset) {
    (void)addr; (void)len; (void)prot; (void)flags; (void)fd; (void)offset;
    return MAP_FAILED;
}

static __inline int munmap(void* addr, size_t len) {
    (void)addr; (void)len;
    return -1;
}

static __inline int madvise(void* addr, size_t len, int advice) {
    (void)addr; (void)len; (void)advice;
    return 0;
}

#ifdef __cplusplus
}
#endif
