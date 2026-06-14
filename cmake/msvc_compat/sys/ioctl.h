// Minimal sys/ioctl.h stub for MSVC
// Provides ioctl, winsize, and related constants for compilation only.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Terminal window size structure
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

// ioctl request codes
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define FIONREAD    0x4004667F

// fcntl constants
#define F_SETFL     4
#define O_NONBLOCK  0x8000

// fd_set and select (Windows has these in winsock2.h, but that pulls in too much)
// Simple stubs for compilation:
#ifndef FD_SETSIZE
#define FD_SETSIZE 64
#endif

typedef struct {
    unsigned int fd_count;
    int fd_array[FD_SETSIZE];
} fd_set;

static __inline void FD_ZERO(fd_set* set) {
    set->fd_count = 0;
}
static __inline void FD_SET(int fd, fd_set* set) {
    if (set->fd_count < FD_SETSIZE)
        set->fd_array[set->fd_count++] = fd;
}
static __inline int FD_ISSET(int fd, fd_set* set) {
    for (unsigned int i = 0; i < set->fd_count; i++)
        if (set->fd_array[i] == fd)
            return 1;
    return 0;
}

static __inline int select(int nfds, fd_set* rfds, fd_set* wfds, fd_set* efds, struct timeval* timeout) {
    (void)nfds; (void)rfds; (void)wfds; (void)efds; (void)timeout;
    return -1;
}

// Stub ioctl
static __inline int ioctl(int fd, unsigned long request, ...) {
    (void)fd; (void)request;
    return -1;
}

// fcntl stub
static __inline int fcntl(int fd, int cmd, ...) {
    (void)fd; (void)cmd;
    return -1;
}

#ifdef __cplusplus
}
#endif
