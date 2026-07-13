cmake_minimum_required(VERSION 3.20)
project(libvterm LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── 源文件 ──────────────────────────────────────────────
# libvterm 包含 9 个 C 源文件，.inc 文件被 encoding.c 通过
# #include 包含，不需要单独编译。
set(VTERM_SOURCES
    src/vterm.c
    src/parser.c
    src/state.c
    src/screen.c
    src/keyboard.c
    src/mouse.c
    src/pen.c
    src/encoding.c
    src/unicode.c
)

add_library(libvterm STATIC ${VTERM_SOURCES})

# 公开头文件（外部通过 #include <vterm.h> 引用）
target_include_directories(libvterm
    PUBLIC  include
    PRIVATE src                   # vterm_internal.h / rect.h / utf8.h / *.inc
)

# 隐藏内部符号，仅导出 vterm.h 中声明的 API
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # 与原始 Makefile 保持一致的警告级别
    target_compile_options(libvterm PRIVATE -Wall -Wpedantic)
    
    target_compile_options(libvterm PRIVATE -fvisibility=hidden)
endif()
