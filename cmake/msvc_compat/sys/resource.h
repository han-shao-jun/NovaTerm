// Minimal sys/resource.h stub for MSVC
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define RUSAGE_SELF 0

struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
};

static __inline int getrusage(int who, struct rusage* usage) {
    (void)who; (void)usage;
    return 0;
}

#ifdef __cplusplus
}
#endif
