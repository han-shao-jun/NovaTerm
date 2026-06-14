// Minimal termios.h stub for MSVC
// Provides termios structure and constants for compilation only.
// Terminal I/O attributes don't apply on Windows — our PtyTransport
// uses QProcess instead of kpty for local shells.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Input flags
#define IGNBRK  0x0001
#define BRKINT  0x0002
#define IGNPAR  0x0004
#define PARMRK  0x0008
#define INPCK   0x0010
#define ISTRIP  0x0020
#define INLCR   0x0040
#define IGNCR   0x0080
#define ICRNL   0x0100
#define IXON    0x0400
#define IXOFF   0x0800
#define IUCLC   0x1000

// Output flags
#define OPOST   0x0001

// Local flags
#define ECHO    0x0008
#define ECHOE   0x0010
#define ECHOK   0x0020
#define ECHONL  0x0040
#define ICANON  0x0100
#define IEXTEN  0x8000

// Control flags
#define CS8     0x0030

// c_cc indices
#define VINTR   0
#define VQUIT   1
#define VERASE  2
#define VKILL   3
#define VEOF    4
#define VTIME   5
#define VMIN    6
#define NCCS    16

typedef unsigned char cc_t;
typedef unsigned int  speed_t;
typedef unsigned int  tcflag_t;

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_cc[NCCS];
    speed_t  c_ispeed;
    speed_t  c_ospeed;
};

// TCSANOW constant
#define TCSANOW 0

// Stub functions
static __inline int tcgetattr(int fd, struct termios* t) {
    (void)fd; (void)t;
    return -1;
}

static __inline int tcsetattr(int fd, int opt, const struct termios* t) {
    (void)fd; (void)opt; (void)t;
    return -1;
}

#ifdef __cplusplus
}
#endif
