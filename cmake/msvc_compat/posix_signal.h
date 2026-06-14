// Minimal POSIX signal stub for MSVC
// Provides signal constants, types, and stub functions for compilation.
// These are NOT functional — signal semantics don't apply on Windows.
#pragma once

// POSIX signal numbers that MSVC doesn't define
#ifndef SIGHUP
#define SIGHUP  1
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef SIGTERM
#define SIGTERM 15
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGCHLD
#define SIGCHLD 17
#endif
#ifndef SIGINT
#define SIGINT  2
#endif
#ifndef SIGQUIT
#define SIGQUIT 3
#endif

// pid_t
#ifdef _WIN32
#ifndef pid_t
typedef int pid_t;
#endif
#endif

// sigaction structure
struct sigaction {
    void (*sa_handler)(int);
    int sa_flags;
};
#define SA_NOCLDSTOP 0

// sigset_t stub
typedef int sigset_t_int;

// Minimal stub functions
#ifdef __cplusplus
extern "C" {
#endif

static __inline int kill(int pid, int sig) {
    (void)pid; (void)sig;
    return 0;
}

static __inline int sigemptyset(sigset_t_int* set) {
    (void)set;
    return 0;
}

static __inline int tcgetpgrp(int fd) {
    (void)fd;
    return 0;
}

#ifdef __cplusplus
}
#endif
