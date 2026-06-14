// Minimal unistd.h stub for MSVC
// Provides POSIX types/functions used by QTermWidget on Windows.
//
// IMPORTANT: Do NOT #define write/read/close — those collide with Qt method names.
// Do NOT include Windows.h or similar — they break with BEFORE SYSTEM include order.

#pragma once

// Only use CRT headers here — NO Windows SDK headers (no synchapi.h, windows.h, etc.)
#include <io.h>
#include <process.h>
#include <direct.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

// ssize_t — signed size type, not defined by MSVC
#ifdef _WIN64
typedef __int64 ssize_t;
#else
typedef int ssize_t;
#endif

// R_OK, W_OK etc.
#ifndef R_OK
#define R_OK 4
#define W_OK 2
#define X_OK 1
#define F_OK 0
#endif

// Safe mappings (won't collide with Qt method names)
#define unlink(path)         _unlink(path)
#define getpid()             _getpid()
#define isatty(fd)           _isatty(fd)

// chdir, getcwd are C runtime functions — use the MSVC names
#define chdir(path)          _chdir(path)
#define getcwd(buf, size)    _getcwd(buf, size)

// access — MSVC uses _access
#define access(path, mode)   _access(path, mode)

// usleep — use a simple inline approach without Windows.h
// Files that actually need sleep functionality should include synchapi.h
// AFTER Qt headers and call Sleep() directly.
// #define usleep(us)           // intentionally not defined — use Sleep()

// File mode flags
#ifndef S_IWGRP
#define S_IWGRP 0
#endif
#ifndef S_IWOTH
#define S_IWOTH 0
#endif
