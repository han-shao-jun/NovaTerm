// Minimal sys/time.h stub for MSVC
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct timeval {
    long tv_sec;
    long tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

#include <time.h>

static __inline int gettimeofday(struct timeval* tv, struct timezone* tz) {
    (void)tz;
    if (tv) {
        // Use _time64 for approximate implementation
        tv->tv_sec = (long)time(NULL);
        tv->tv_usec = 0;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif
