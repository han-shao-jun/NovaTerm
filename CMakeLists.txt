cmake_minimum_required(VERSION 3.20)
project(NovaTerm VERSION 0.2.62 LANGUAGES CXX C)

# ═══════════════════════════════════════════════════════════════════
#  1. 通用平台编译参数
# ═══════════════════════════════════════════════════════════════════

# 主工程显式采用平台标准安装目录，避免依赖第三方子项目间接初始化这些变量。
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTORCC ON)

# Ninja 必须能识别 /showIncludes 前缀。中文诊断的代码页可能与 build.ninja 的
# UTF-8 不同，导致依赖记录为 0、头文件更新后继续链接旧对象。统一使用 ASCII。
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    find_program(_novaterm_powershell NAMES powershell REQUIRED)
    file(SHA256 "${PROJECT_SOURCE_DIR}/scripts/msvc-deps-launcher.cmake.in" _novaterm_launcher_hash)
    file(SHA256 "${PROJECT_SOURCE_DIR}/scripts/normalize-msvc-output.ps1" _novaterm_normalizer_hash)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/scripts/normalize-msvc-output.ps1")
    configure_file(scripts/msvc-deps-launcher.cmake.in
        ${CMAKE_BINARY_DIR}/msvc-deps-launcher.cmake @ONLY)
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "Note: including file: ")
    set(CMAKE_C_CL_SHOWINCLUDES_PREFIX "Note: including file: ")
    set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "Note: including file: ")
    list(PREPEND CMAKE_C_COMPILER_LAUNCHER ${CMAKE_COMMAND}
        -DLAUNCHER_REV=${_novaterm_launcher_hash}_${_novaterm_normalizer_hash}
        -P ${CMAKE_BINARY_DIR}/msvc-deps-launcher.cmake --)
    list(PREPEND CMAKE_CXX_COMPILER_LAUNCHER ${CMAKE_COMMAND}
        -DLAUNCHER_REV=${_novaterm_launcher_hash}_${_novaterm_normalizer_hash}
        -P ${CMAKE_BINARY_DIR}/msvc-deps-launcher.cmake --)
endif()

# 将所有运行时文件（exe、dll）输出到同一目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 将所有库文件（.so、.dylib）也输出到同一目录
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Windows 上尤其重要：DLL 和 exe 必须在一起
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)


# ═══════════════════════════════════════════════════════════════════
#  2. 跨平台编译参数
# ═══════════════════════════════════════════════════════════════════

# ── Qt SDK 前缀：按宿主平台定位本地安装 ──────────────────────────
# 注意：此处使用 CMAKE_HOST_SYSTEM_NAME（构建机），因为 Qt SDK 装在构建机上；
# 下方的 RPATH / install / CPack 等规则使用 CMAKE_SYSTEM_NAME（目标平台）。
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_PREFIX_PATH /home/super/Qt/6.8.3/gcc_64/ CACHE PATH "Qt installation prefix")
    message(STATUS "Host OS: Linux")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(CMAKE_PREFIX_PATH "C:\\Programs\\Qt\\6.8.3\\msvc2022_64" CACHE PATH "Qt installation prefix")
    message(STATUS "Host OS: Windows")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(CMAKE_PREFIX_PATH "/opt/Qt/6.8.3/macos" CACHE PATH "Qt installation prefix")
    message(STATUS "Host OS: macOS")
endif()

message(STATUS "setting qt sdk path: ${CMAKE_PREFIX_PATH}")

# Find Qt
# 注意：Qt6::Sql 已从依赖中移除——项目当前使用 MemoryProfileStore / 平台密钥链，
# 不依赖 QSqlDatabase。若将来引入 SQLite 等持久化存储，需重新加回 Sql 组件。
# Qt6::DBus 只在非 Apple 的 Unix 上需要：那里的凭据持久化走 freedesktop Secret
# Service（见 src/credential/CredentialStore.cpp），Windows 用 Credential Manager。
set(_novaterm_secret_service_component)
if(UNIX AND NOT APPLE)
    list(APPEND _novaterm_secret_service_component DBus)
endif()

find_package(QT 6.8 NAMES Qt6 REQUIRED COMPONENTS Widgets Svg ShaderTools)
find_package(Qt${QT_VERSION_MAJOR} 6.8 REQUIRED COMPONENTS
    Widgets
    Svg
    ShaderTools
    LinguistTools
    SerialPort
    Network
    ${_novaterm_secret_service_component}
)
qt_standard_project_setup(REQUIRES 6.8)

message(STATUS "Using Qt ${QT_VERSION_MAJOR}.${QT_VERSION_MINOR}")

# 凭据后端需要额外链接的 Qt 模块（供 tests/ 里的目标复用）。
if(_novaterm_secret_service_component)
    set(NOVATERM_SECRET_SERVICE_LIBS Qt${QT_VERSION_MAJOR}::DBus)
else()
    set(NOVATERM_SECRET_SERVICE_LIBS)
endif()

# ElaWidgetTools - built as subdirectory
# 通过选项关闭其开发期 install（头文件 / cmake 配置 / 导入库）与示例程序构建，
# 避免混入运行时分发包；NovaTerm 所需的动态库由主项目显式安装。
# 注：CMAKE_SKIP_INSTALL_RULES 是全局开关，无法按子目录屏蔽，故改用选项守卫。
set(ElaWidgetTools_INSTALL OFF CACHE BOOL "Install ElaWidgetTools dev files (headers/cmake/export)" FORCE)
set(ElaWidgetTools_BUILD_EXAMPLE OFF CACHE BOOL "Build ElaWidgetTools example application" FORCE)

# ElaWidgetTools 的独立构建配置会强制修改全局 CMAKE_INSTALL_PREFIX。作为子项目
# 引入时保存并恢复主工程的值，防止 `cmake --install` 被重定向到源码目录。
set(_novaterm_install_prefix "${CMAKE_INSTALL_PREFIX}")
add_subdirectory(third_party/ElaWidgetTools)
# Ela 的宏在消费方的 -Wpedantic 下会触发扩展诊断；将其导出头目录标为系统头。
target_include_directories(ElaWidgetTools SYSTEM INTERFACE
    "${PROJECT_SOURCE_DIR}/third_party/ElaWidgetTools/ElaWidgetTools"
    "${PROJECT_SOURCE_DIR}/third_party/ElaWidgetTools/ElaWidgetTools/private"
    "${PROJECT_SOURCE_DIR}/third_party/ElaWidgetTools/ElaWidgetTools/DeveloperComponents"
    "${PROJECT_SOURCE_DIR}/third_party/ElaWidgetTools/ElaWidgetTools/DeveloperComponents/Command"
)
set(CMAKE_INSTALL_PREFIX "${_novaterm_install_prefix}" CACHE PATH
    "Installation prefix" FORCE)
unset(_novaterm_install_prefix)

# libvterm — VT/ANSI 终端解析器静态库（纯 C99，无外部依赖）
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/libvterm-0.3.3/include/vterm.h")
    message(STATUS "Building libvterm from third_party/libvterm-0.3.3")
    add_subdirectory(third_party/libvterm-0.3.3)
    # 纯 C 库不需要 Qt 的自动 moc/uic/rcc，也不应生成额外的 C++ 编译单元。
    set_target_properties(libvterm PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
else()
    message(FATAL_ERROR "libvterm not found at third_party/libvterm-0.3.3")
endif()

# libtelnet — TELNET 协议状态机静态库（纯 C，public domain，无外部依赖）。
# 只做协议解析与选项协商（RFC 854/855/1091/1143/1572），不持有 socket；
# TelnetTransport 把 QTcpSocket 的字节喂给它，再把事件转回 ITransport 信号。
#
# 刻意不用 add_subdirectory：上游 CMakeLists 会连带构建 util/doc/test 示例，
# 并执行 install(TARGETS/EXPORT) 与 include(CPack)，后者会覆盖 NovaTerm 自己的
# 打包配置。这里直接从 vendored 源码声明目标，third_party/libtelnet 工作副本
# 保持零改动（它是一份 git clone，本地改动会造成后续 pull 冲突）。
set(_libtelnet_dir "${CMAKE_SOURCE_DIR}/third_party/libtelnet")
if(NOT EXISTS "${_libtelnet_dir}/libtelnet.c")
    message(FATAL_ERROR "libtelnet not found at third_party/libtelnet")
endif()
message(STATUS "Building libtelnet from third_party/libtelnet")
add_library(libtelnet STATIC "${_libtelnet_dir}/libtelnet.c")
target_include_directories(libtelnet PUBLIC "${_libtelnet_dir}")
# 纯 C 目标，无需 Qt 的自动代码生成（否则会额外编出一个 C++ 目标）。
set_target_properties(libtelnet PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
# 刻意不在 MSVC 上设置 C_STANDARD：/std:c11 会让 MSVC 定义
# __STDC_VERSION__ >= 199901L，从而命中上游 INLINE 宏的 GNU 分支
#   #if defined(__GNUC__) || __STDC_VERSION__ >= 199901L
#   # define INLINE __inline__
# 而 __inline__ 是 GCC 拼写，MSVC 无法解析（C2054）。MSVC 默认 C 模式不定义
# 该宏，INLINE 退化为空，编译正常。GCC/Clang 走 __GNUC__ 分支，不受影响。
if(NOT MSVC)
    set_target_properties(libtelnet PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
    )
endif()
# HAVE_ZLIB 保持不定义 ⇒ 不编译 MCCP2 压缩（MUD 专用），因此不需要 zlib。
if(MSVC)
    # 上游在 _WIN32 下把 vsnprintf 映射为已弃用的 _vsnprintf（触发 C4996）。
    # 该映射只影响 telnet_printf()/telnet_raw_printf()，TelnetTransport 不使用
    # 这两个接口（改用 telnet_send/telnet_iac/telnet_subnegotiation）。
    target_compile_definitions(libtelnet PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(libtelnet PRIVATE -Wall -fvisibility=hidden)
endif()
unset(_libtelnet_dir)

# Clink (chrisant996/clink v1.9.27.83514e) —— 运行时工具，不参与编译。
# 预编译的 release 目录 third_party/clink.1.9.27.83514e 在构建后事件中
# 复制到 bin/（与 NovaTerm.exe 同级）。TerminalView::startLocalShell()
# 的 cmd 类型通过 cmd.exe /k "clink.bat" inject 启动增强 cmd 会话；
# 找不到 clink.bat 时回退裸 cmd.exe。

# libssh — SSH 会话传输。直接以源码子目录方式构建（不再 find_package 预编译产物）。
# OpenSSL：Windows 使用 vendored 一次性预编译（scripts/build-OpenSSL-thirdparty.bat 编译，
# 产物在 third_party/openssl-3.5.7/install）；其他平台（Linux/macOS）直接用系统
# OpenSSL（apt/dnf 安装的 libssl-dev / openssl-devel），由下方 find_package 解析。
if(WIN32)
    set(OPENSSL_INSTALL_DIR
        "${CMAKE_SOURCE_DIR}/third_party/openssl-3.5.7/install"
        CACHE PATH "OpenSSL install prefix (run scripts/build-OpenSSL-thirdparty.bat)")
    # 必须用 OPENSSL_ROOT_DIR（而非 find_package 的 HINTS）：libssh 子目录内部的
    # find_package(OpenSSL 1.1.1) 不带 HINTS，只有 MODULE 模式才认 OPENSSL_ROOT_DIR；
    # 若走 OpenSSL 自带的 OpenSSLConfig.cmake（CONFIG 模式）则不会写 OPENSSL_* cache，
    # 导致 libssh 找不到 libcrypto。
    set(OPENSSL_ROOT_DIR "${OPENSSL_INSTALL_DIR}" CACHE PATH
        "OpenSSL root dir consumed by FindOpenSSL (module mode) and the libssh subdirectory")
endif()
find_package(OpenSSL REQUIRED)

set(BUILD_SHARED_LIBS OFF)   # 静态 libssh（与预编译 ssh.lib 配置一致）
set(WITH_EXAMPLES OFF)
set(WITH_SERVER  OFF)
set(WITH_GSSAPI  OFF)
set(WITH_ZLIB    OFF)
set(WITH_NACL    OFF)
set(WITH_PCAP    OFF)
set(WITH_SFTP    ON)
set(UNIT_TESTING OFF)
set(CLIENT_TESTING OFF)
set(SERVER_TESTING OFF)
set(WITH_BENCHMARKS OFF)
add_subdirectory(third_party/libssh-0.12.2 EXCLUDE_FROM_ALL)
set_target_properties(ssh PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
# libssh 自带的生成 C99 源码与旧接口兼容层不符合其对普通源码的告警策略。
# 仅对这三个上游文件关闭对应诊断，保留其余 libssh 警告检查。
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(
        "${CMAKE_SOURCE_DIR}/third_party/libssh-0.12.2/src/external/libcrux_mlkem768_sha3.c"
        TARGET_DIRECTORY ssh PROPERTIES COMPILE_OPTIONS "-Wno-declaration-after-statement")
    set_source_files_properties(
        "${CMAKE_SOURCE_DIR}/third_party/libssh-0.12.2/src/legacy.c"
        "${CMAKE_SOURCE_DIR}/third_party/libssh-0.12.2/src/scp.c"
        TARGET_DIRECTORY ssh PROPERTIES COMPILE_OPTIONS "-Wno-deprecated-declarations")
endif()

# 第一方 C++ 目标统一使用相同的编码和严格警告策略。此处位于 vendored 子目录之后，
# 避免把项目编译策略传入 ElaWidgetTools、libvterm 与 libssh。
#
# NOMINMAX 必须在这里全局定义，而不是逐个文件加 `#ifndef NOMINMAX` 守卫：
# windows.h 的 min/max 是无作用域宏，会让 `std::min(a, b)` 变成
# `std::min((a), (b))` 之外的任意形态直到编译失败，而 windows.h 经常是
# **间接**包含进来的（ConPtyApi.h、serialport_info.h、WinHandle.h、
# CredentialStore.cpp、SettingsPage.cpp、main.cpp 等十余个文件经由平台头引入）。
# 逐文件守卫只能覆盖直接包含者，且新增文件必然漏掉；命令行 /DNOMINMAX 一次性
# 覆盖所有第一方 TU，包括以后新增的。当前 src/ 下有 132 处 std::min/max
# 写法，若某天某个 TU 先包含了 windows.h 就是编译失败。
add_compile_definitions(
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:NOMINMAX>"
)
add_compile_options(
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/utf-8>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/Wall>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/WX>"
    # /Wall 额外启用的这批警告无 actionable 价值且随工具集版本波动
    # （对齐填充、内联决策、隐式删除的拷贝/移动），保留 /WX 对其余
    # 警告仍然严格。
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/wd4514>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/wd4710>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/wd4711>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/wd4820>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/wd4625>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/wd4626>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/wd5026>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/wd5027>"
    # C4365(符号转换) 对应 GCC 的 -Wsign-conversion —— 不在 -Wall/-Wextra
    # 内，压掉后 MSVC 与 Linux 构建的严格度对齐。
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/wd4365>"
    # AUTOMOC 生成文件用相对路径 ".." 引用头文件，无法在源码侧修复。
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/wd4464>"
    # C5045 纯提示性（"若指定 /Qspectre 会插入缓解"），无行动价值。
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/wd5045>"
    # 以下三类同为 /Wall 独有的理论性警告，GCC -Wall/-Wextra 均无对应：
    # C4061 switch 有 default 分支但未显式列出全部枚举值；
    # C4868 大括号初始化列表的理论求值顺序；C5039 C 回调理论可抛异常。
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/wd4061>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/wd4868>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/wd5039>"
    # 其余 /Wall 独有噪音：C5219 函数指针隐式转换、C4866 下标求值顺序、
    # C4191 GetProcAddress 类 API 的函数指针强转、C4668 vendored 头里的
    # 未定义宏判断（libtelnet）。
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/wd5219>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/wd4866>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/wd4191>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/wd4668>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-Wall>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-Wextra>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-Wpedantic>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>>:-Werror>"
    "$<$<AND:$<CONFIG:RelWithDebInfo>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>>:-fno-omit-frame-pointer>"
)

# Windows 平台标识与跨平台 Release 日志策略适用于全部第一方 C++ 目标。
add_compile_definitions(
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<PLATFORM_ID:Windows>>:_WIN32>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<PLATFORM_ID:Windows>>:_WIN64>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>>:QT_NO_DEBUG_OUTPUT>"
)

set(NOVATERM_CORE_SOURCES
    src/core/ThreadNaming.cpp
    src/core/ThreadNaming.h
    src/core/terminal/BoundedByteQueue.cpp
    src/core/terminal/BoundedByteQueue.h
    src/core/terminal/KeyMapper.cpp
    src/core/terminal/KeyMapper.h
    src/core/terminal/ScreenBuffer.cpp
    src/core/terminal/ScreenBuffer.h
    src/core/terminal/ScrollbackBuffer.cpp
    src/core/terminal/ScrollbackBuffer.h
    src/core/terminal/TerminalCore.cpp
    src/core/terminal/TerminalCore.h
    src/core/terminal/TerminalTypes.h
    src/core/terminal/VTAdapter.cpp
    src/core/terminal/VTAdapter.h
    src/core/scrollback/ChunkedScrollback.cpp
    src/core/scrollback/ChunkedScrollback.h
    src/core/scrollback/LineLayout.cpp
    src/core/scrollback/LineLayout.h
    src/core/scrollback/ScrollbackChunk.cpp
    src/core/scrollback/ScrollbackChunk.h
    src/core/scrollback/ScrollbackSnapshot.cpp
    src/core/scrollback/ScrollbackSnapshot.h
    src/core/scrollback/ScrollbackTypes.h
    src/core/search/SearchEngine.cpp
    src/core/search/SearchEngine.h
)

add_library(novaterm_core STATIC ${NOVATERM_CORE_SOURCES})

target_include_directories(novaterm_core
    PUBLIC
        src
    PRIVATE
        third_party/libvterm-0.3.3/include
)

target_link_libraries(novaterm_core
    PUBLIC
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui
    PRIVATE
        libvterm
)

# P9 协议库独立构建；串口接线前不链接进 NovaTerm。
add_subdirectory(src/filetransfer)

# Qt 门面只负责串口通道与异步文件宿主，纯协议库继续可独立使用。
qt_add_library(novaterm_serial_transfer STATIC
    src/session/transfer/SerialTransferTypes.h
    src/session/transfer/SerialFileTransferController.h
    src/session/transfer/SerialFileTransferController.cpp
    src/session/transfer/TransferFileWorker.h
    src/session/transfer/TransferFileWorker.cpp
)
target_include_directories(novaterm_serial_transfer PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(novaterm_serial_transfer PUBLIC
    novaterm_filetransfer Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::SerialPort
    PRIVATE novaterm_core
)


file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp" "src/*.h")
list(FILTER SOURCES EXCLUDE REGEX "[/\\\\]src[/\\\\]core[/\\\\]")
list(FILTER SOURCES EXCLUDE REGEX "[/\\\\]src[/\\\\]mcp[/\\\\]")
list(FILTER SOURCES EXCLUDE REGEX "[/\\\\]src[/\\\\]filetransfer[/\\\\]")
list(FILTER SOURCES EXCLUDE REGEX "[/\\\\]src[/\\\\]session[/\\\\]transfer[/\\\\]")
list(FILTER SOURCES EXCLUDE REGEX "[/\\\\]SessionDirectory\\.(cpp|h)$")
list(FILTER SOURCES EXCLUDE REGEX
    "[/\\\\](CommandPlatformProfile|ISessionCommandExecutor|LocalSessionCommandExecutor|SessionCommandFacade|SshSessionCommandExecutor)\\.(cpp|h)$"
)
add_subdirectory(tools/novaterm-local-diag)
add_subdirectory(src/mcp)
# MCP 桥接与主程序同目录，供运行时按 applicationDirPath 定位。
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_novaterm_linux_runtime_root "${CMAKE_INSTALL_LIBDIR}/novaterm")
    set(_novaterm_linux_bindir "${_novaterm_linux_runtime_root}/bin")
    set(_novaterm_linux_libdir "${_novaterm_linux_runtime_root}/lib")
    set(_novaterm_linux_pluginsdir "${_novaterm_linux_runtime_root}/plugins")
endif()
add_subdirectory(tools/novaterm-mcp)
if(WIN32)
    list(FILTER SOURCES EXCLUDE REGEX "[/\\\\]src[/\\\\]platform[/\\\\]linux[/\\\\]")
    list(APPEND SOURCES resources/novaterm.rc)
else()
    list(FILTER SOURCES EXCLUDE REGEX "[/\\\\]src[/\\\\]platform[/\\\\]windows[/\\\\]")
endif()

qt_add_executable(NovaTerm
    ${SOURCES}
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)
add_dependencies(NovaTerm novaterm-local-diag)

# 主程序与直接编译配色服务的测试使用同一份 JSON 模板。
function(novaterm_add_scheme_resources target)
    qt_add_resources(${target} "${target}_scheme_presets"
        PREFIX "/novaterm"
        BASE "${PROJECT_SOURCE_DIR}/resources"
        FILES "${PROJECT_SOURCE_DIR}/resources/terminal-color-schemes.json"
    )
endfunction()
novaterm_add_scheme_resources(NovaTerm)

target_include_directories(NovaTerm PRIVATE
    src
    third_party/libvterm-0.3.3/include
)
target_compile_definitions(NovaTerm PRIVATE
    NOVATERM_VERSION="${PROJECT_VERSION}"
)

if(MSVC)
    target_compile_options(NovaTerm PRIVATE
        $<$<CONFIG:Release>:/Zi>
    )

    target_link_options(NovaTerm PRIVATE
        $<$<CONFIG:Release>:/DEBUG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()
target_link_libraries(NovaTerm PRIVATE
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Svg
        Qt${QT_VERSION_MAJOR}::SerialPort
        Qt${QT_VERSION_MAJOR}::Network
        ${NOVATERM_SECRET_SERVICE_LIBS}
        ElaWidgetTools
        novaterm_core
        novaterm_serial_transfer
        novaterm_mcp_runtime
        libtelnet
        ssh::ssh
        OpenSSL::Crypto
        OpenSSL::SSL
)

# 静态 OpenSSL 在 Windows 上的平台依赖库
if(WIN32)
    target_link_libraries(NovaTerm PRIVATE ws2_32 crypt32 user32 advapi32)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(NovaTerm PRIVATE udev util)
endif()


qt_add_shaders(NovaTerm novaterm_renderer_shaders
    PREFIX
        "/shaders"
    FILES
        src/renderer/shaders/terminal_texture.vert
        src/renderer/shaders/terminal_texture.frag
)

# ── 跨平台可执行文件属性与运行时部署 ─────────────────────────────
# Windows: WIN32_EXECUTABLE 去除控制台黑窗口（仅 Release）
# macOS  : MACOSX_BUNDLE 生成 .app 包结构并嵌入 Info.plist
if(WIN32)
    if(CMAKE_BUILD_TYPE MATCHES "Release")
        # 发布版除去控制台窗口。
        set_target_properties(NovaTerm PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set_target_properties(NovaTerm PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "NovaTerm"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.novaterm"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/Info.plist.in"
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Debian 可执行文件使用小写名称，与包名及 desktop 文件中的 Exec 一致。
    # Qt、ICU 与 ElaWidgetTools 等随包运行库放入应用私有目录，避免污染或覆盖
    # 发行版在 /usr/lib 中管理的同名库。
    set_target_properties(NovaTerm PROPERTIES
        OUTPUT_NAME novaterm
        INSTALL_RPATH "\$ORIGIN/../lib"
    )
    set_target_properties(novaterm-mcp PROPERTIES
        INSTALL_RPATH "\$ORIGIN/../lib")
    # 覆盖 ElaWidgetTools 子项目中面向独立构建的硬编码 Windows Qt RPATH。
    set_target_properties(ElaWidgetTools PROPERTIES
        INSTALL_RPATH "\$ORIGIN"
    )
endif()


set(TS_FILES
        resources/translations/novaterm_en.ts
        resources/translations/novaterm_zh_CN.ts
)

# 为 Qt6 添加翻译文件，使用翻译工具扫描代码中的字符串生成ts文件
# 然后使用翻译工具翻译ts文件，生成qm文件。
# LUPDATE_OPTIONS -no-obsolete：跑 `cmake --build <build> --target update_translations`
# 时同步源码里已删除/移走的字符串（清掉 type="vanished" 的旧条目），避免 .ts 与源码漂移。
qt_add_translations(
        TARGETS NovaTerm
        TS_FILES ${TS_FILES}
        LUPDATE_OPTIONS -no-obsolete
)

# .qm 已由 qt_add_translations 嵌入可执行文件，LanguageManager 也只从资源路径
# 读取应用翻译，因此不再重复安装一份不会被程序加载的外部翻译文件。

# 通过构建系统运行 `test` 目标时，始终显示测试命令及其输出；失败时保留
# 完整日志，避免 CTest 默认摘要隐藏 Qt Test 给出的断言位置和诊断信息。
set(CMAKE_CTEST_ARGUMENTS
    "--output-on-failure"
    "--verbose"
    "--progress"
)

include(CTest)

# 测试、人工检查与性能基准由子目录独立维护。
add_subdirectory(tests)

# ═══════════════════════════════════════════════════════════════════
#  3. 编译运行期间需要的运行库
# ═══════════════════════════════════════════════════════════════════

# Clink 预编译 release 路径。测试目标在 tests/CMakeLists.txt 中独立引用同一目录。
if(WIN32)
    set(CLINK_SRC_DIR "${CMAKE_SOURCE_DIR}/third_party/clink.1.9.27.83514e")
endif()

# ── Windows: 复制 Clink 到构建输出，支持直接从构建目录运行 ─────
if(WIN32)
    if(EXISTS "${CLINK_SRC_DIR}/clink.bat")
        add_custom_command(TARGET NovaTerm POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CLINK_SRC_DIR}/clink.bat"
                "${CLINK_SRC_DIR}/clink_x64.exe"
                "${CLINK_SRC_DIR}/clink_dll_x64.dll"
                "${CLINK_SRC_DIR}/clink.html"
                $<TARGET_FILE_DIR:NovaTerm>
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:NovaTerm>/clink-scripts"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/resources/clink/novaterm_prompt.lua"
                "$<TARGET_FILE_DIR:NovaTerm>/clink-scripts/novaterm_prompt.lua"
            COMMENT "Copying Clink release files next to NovaTerm.exe"
        )
        message(STATUS "Clink release will be deployed to output dir: ${CLINK_SRC_DIR}")
    else()
        message(WARNING "Clink release not found at ${CLINK_SRC_DIR} — cmd sessions will use bare cmd.exe")
    endif()
endif()

# 平台相关的部署工具选项。构建后部署（windeployqt POST_BUILD）与安装阶段的
# qt_generate_deploy_app_script 共用同一份，避免两处取舍逐渐分叉。
set(_deploy_tool_options)
if(WIN32)
    list(APPEND _deploy_tool_options
        --no-system-d3d-compiler
        --no-opengl-sw
    )
endif()

# ── Windows: 用 windeployqt 将 Qt 运行时 DLL 与插件复制到构建输出 bin/ ──
# 构建完成后即可直接运行 ${CMAKE_BINARY_DIR}/bin/NovaTerm.exe，无需把 Qt 的
# bin 目录加入 PATH。exe、ElaWidgetTools.dll 与 Clink 已经统一输出到该目录，
# windeployqt 部署到 exe 同级，因此运行时文件集中在一处。
#   --no-translations     .qm 已由 qt_add_translations 编入可执行文件
#   --no-compiler-runtime MSVC CRT 只在安装阶段按 App-local 规则处理，
#                         开发机运行构建产物直接用已安装的 VC 运行库
if(WIN32)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt
        HINTS "${CMAKE_PREFIX_PATH}/bin" "${QT6_INSTALL_PREFIX}/bin"
    )
    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET NovaTerm POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                $<IF:$<CONFIG:Debug>,--debug,--release>
                --no-translations
                --no-compiler-runtime
                ${_deploy_tool_options}
                "$<TARGET_FILE:NovaTerm>"
            COMMENT "Deploying Qt runtime (windeployqt) next to NovaTerm.exe"
            VERBATIM
        )
        message(STATUS "windeployqt found: ${WINDEPLOYQT_EXECUTABLE}")
    else()
        message(WARNING "windeployqt not found — Qt DLLs will not be auto-deployed")
    endif()
endif()


# ── Qt 官方基础翻译（qtbase_<lang>.qm）───────────────────────────
# LanguageManager 运行时会从 exe 同级的 translations/ 目录加载 qtbase 翻译，
# 用于覆盖 QFileDialog / QMessageBox / QLineEdit 右键菜单等标准控件的内置文案。
# 构建后复制供开发目录直接运行；安装规则在第 5 节按平台设置目标目录。
find_file(_NOVATERM_QTBASE_ZH_QM
    qtbase_zh_CN.qm
    PATHS "${CMAKE_PREFIX_PATH}/translations"
    NO_DEFAULT_PATH
)
if(_NOVATERM_QTBASE_ZH_QM)
    add_custom_command(TARGET NovaTerm POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_NOVATERM_QTBASE_ZH_QM}"
            "$<TARGET_FILE_DIR:NovaTerm>/translations/qtbase_zh_CN.qm"
        COMMENT "Copying Qt qtbase_zh_CN.qm next to NovaTerm"
        VERBATIM
    )
    message(STATUS "Qt qtbase_zh_CN.qm: ${_NOVATERM_QTBASE_ZH_QM}")
else()
    message(WARNING "qtbase_zh_CN.qm not found under CMAKE_PREFIX_PATH/translations — standard Qt dialogs stay English in zh_CN UI")
endif()

# ═══════════════════════════════════════════════════════════════════
#  4. 通用平台安装规则
# ═══════════════════════════════════════════════════════════════════

# Linux 的真实程序和 qt.conf 同处私有 bin，公共 bin 只放启动器，避免其他
# Qt 程序读取 NovaTerm 的 qt.conf。启动器使用相对路径，支持 DEB 和 ZIP。
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    install(TARGETS NovaTerm RUNTIME DESTINATION "${_novaterm_linux_bindir}")
    file(RELATIVE_PATH _novaterm_linux_launcher_target
        "/${CMAKE_INSTALL_BINDIR}"
        "/${_novaterm_linux_bindir}/novaterm"
    )
    set(_novaterm_linux_launcher_content [=[#!/bin/sh
set -eu
launcher_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$launcher_dir/@_novaterm_linux_launcher_target@" "$@"
]=])
    string(CONFIGURE "${_novaterm_linux_launcher_content}"
        _novaterm_linux_launcher_content @ONLY)
    set(_novaterm_linux_launcher "${CMAKE_CURRENT_BINARY_DIR}/novaterm-launcher")
    file(GENERATE OUTPUT "${_novaterm_linux_launcher}"
        CONTENT "${_novaterm_linux_launcher_content}")
    install(PROGRAMS "${_novaterm_linux_launcher}"
        DESTINATION "${CMAKE_INSTALL_BINDIR}" RENAME novaterm)
else()
    install(TARGETS NovaTerm
        BUNDLE DESTINATION .
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# 在安装阶段收集 Qt 库、插件和平台运行时。Windows/macOS 底层分别调用
# windeployqt/macdeployqt，Linux 使用 CMake 的运行时依赖扫描。
# _deploy_tool_options 已在上方构建后部署处定义，此处直接复用。
qt_generate_deploy_app_script(
    TARGET NovaTerm
    OUTPUT_SCRIPT _novaterm_deploy_script
    NO_TRANSLATIONS
    DEPLOY_TOOL_OPTIONS ${_deploy_tool_options}
)
# ── 通用元数据 ─────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME "novaterm")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VENDOR "NovaTerm Project")
set(CPACK_PACKAGE_CONTACT "li ji")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/han-shao-jun/NovaTerm")

set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "NovaTerm - A modern terminal emulator")
set(CPACK_PACKAGE_DESCRIPTION
    "NovaTerm is a feature-rich terminal emulator built with Qt6 and libvterm.
It supports local and remote terminal sessions with SSH capabilities,
VT/ANSI terminal emulation, and a modern UI powered by ElaWidgetTools."
)

# 资源目录（LICENSE、README 等，按需放入打包根）
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README  "${CMAKE_SOURCE_DIR}/README.md")

# ── 平台与架构标识（用于 CPACK_PACKAGE_FILE_NAME）──────────────
# 将 CMAKE_SYSTEM_PROCESSOR 归一化为发布物常用短名
set(_target_processor "${CMAKE_SYSTEM_PROCESSOR}")
if(NOT _target_processor AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID)
    set(_target_processor "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
endif()

if(_target_processor MATCHES "x86_64|AMD64|x64")
    set(_pkg_arch "x64")
elseif(_target_processor MATCHES "aarch64|arm64|ARM64")
    set(_pkg_arch "arm64")
else()
    set(_pkg_arch "${_target_processor}")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_pkg_os "linux")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(_pkg_os "windows")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(_pkg_os "macos")
else()
    set(_pkg_os "${CMAKE_SYSTEM_NAME}")
endif()

set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${_pkg_os}-${_pkg_arch}")

# ═══════════════════════════════════════════════════════════════════
#  5. 跨平台安装规则
# ═══════════════════════════════════════════════════════════════════

# ElaWidgetTools 是项目自带的动态库。Windows/macOS 的 Qt 部署工具只保证部署
# Qt 运行时，因此显式安装该目标；Linux 将其放入 NovaTerm 的私有库目录。
if(APPLE)
    install(
        TARGETS ElaWidgetTools
        LIBRARY DESTINATION "NovaTerm.app/Contents/Frameworks"
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    install(
        TARGETS ElaWidgetTools
        LIBRARY DESTINATION "${_novaterm_linux_libdir}"
    )
else()
    install(
        TARGETS ElaWidgetTools
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

# Clink 是 Windows cmd 会话的运行时工具，不参与链接，需作为数据文件显式安装。
if(WIN32 AND EXISTS "${CLINK_SRC_DIR}/clink_x64.exe")
    install(
        FILES
            "${CLINK_SRC_DIR}/clink_x64.exe"
            "${CLINK_SRC_DIR}/clink_dll_x64.dll"
            "${CLINK_SRC_DIR}/clink.html"
            "${CLINK_SRC_DIR}/clink.bat"
        DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(FILES "${CMAKE_SOURCE_DIR}/resources/clink/novaterm_prompt.lua"
        DESTINATION "${CMAKE_INSTALL_BINDIR}/clink-scripts")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Qt 生成的 qt.conf 放进主程序私有 bin，并指向私有库和插件目录。
    install(CODE "
        set(QT_DEPLOY_BIN_DIR \"${_novaterm_linux_bindir}\")
        set(QT_DEPLOY_LIB_DIR \"${_novaterm_linux_libdir}\")
        set(QT_DEPLOY_PLUGINS_DIR \"${_novaterm_linux_pluginsdir}\")
        include(\"${_novaterm_deploy_script}\")
    ")
else()
    install(SCRIPT ${_novaterm_deploy_script})
endif()

# 便携式 Windows ZIP 使用 App-local MSVC Runtime。Qt 的
# NO_COMPILER_RUNTIME 只会停止部署 vc_redist 安装器，不会自动复制 CRT DLL。
# 从当前 cl.exe 对应的工具集和目标架构定位 Redist，避免绑定具体 VS 版本。
if(MSVC)
    get_filename_component(_msvc_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(
        _msvc_toolset_dir
        "${_msvc_compiler_dir}/../../.."
        ABSOLUTE
    )
    get_filename_component(_msvc_toolset_version "${_msvc_toolset_dir}" NAME)
    get_filename_component(_msvc_target_arch "${_msvc_compiler_dir}" NAME)
    get_filename_component(
        _msvc_vc_dir
        "${_msvc_toolset_dir}/../../.."
        ABSOLUTE
    )

    file(GLOB _msvc_crt_dirs
        "${_msvc_vc_dir}/Redist/MSVC/${_msvc_toolset_version}/${_msvc_target_arch}/Microsoft.VC*.CRT"
    )
    if(NOT _msvc_crt_dirs)
        message(FATAL_ERROR
            "MSVC App-local runtime not found for toolset ${_msvc_toolset_version} "
            "and architecture ${_msvc_target_arch} under ${_msvc_vc_dir}/Redist/MSVC"
        )
    endif()

    list(GET _msvc_crt_dirs 0 _msvc_crt_dir)
    file(GLOB _msvc_runtime_dlls "${_msvc_crt_dir}/*.dll")
    if(NOT _msvc_runtime_dlls)
        message(FATAL_ERROR "No MSVC runtime DLLs found in ${_msvc_crt_dir}")
    endif()

    install(
        FILES ${_msvc_runtime_dlls}
        DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    message(STATUS "MSVC App-local runtime: ${_msvc_crt_dir}")

    # Qt RHI 会使用 Windows SDK 随附的 DirectX Shader Compiler。显式安装可确保
    # CPack 在未加载 VsDevCmd 环境时仍能生成完整的便携 ZIP。
    get_filename_component(_windows_sdk_bin_dir "${CMAKE_RC_COMPILER}" DIRECTORY)
    set(_directx_runtime_dlls)
    foreach(_directx_dll dxcompiler.dll dxil.dll)
        if(EXISTS "${_windows_sdk_bin_dir}/${_directx_dll}")
            list(APPEND _directx_runtime_dlls
                "${_windows_sdk_bin_dir}/${_directx_dll}"
            )
        else()
            message(WARNING
                "${_directx_dll} not found beside the Windows SDK tools: "
                "${_windows_sdk_bin_dir}"
            )
        endif()
    endforeach()
    if(_directx_runtime_dlls)
        install(
            FILES ${_directx_runtime_dlls}
            DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endif()
endif()

# ── Linux: 桌面入口与图标 ───────────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    install(
        FILES resources/novaterm.desktop
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
    )
    install(
        FILES resources/icons/app_logo.svg
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
        RENAME novaterm.svg
    )
    install(
        FILES LICENSE README.md
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/novaterm"
    )
endif()

# Qt 官方基础翻译跟随真实程序的运行目录安装。
if(_NOVATERM_QTBASE_ZH_QM)
    if(APPLE)
        install(FILES "${_NOVATERM_QTBASE_ZH_QM}"
            DESTINATION "NovaTerm.app/Contents/MacOS/translations"
        )
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        install(FILES "${_NOVATERM_QTBASE_ZH_QM}"
            DESTINATION "${_novaterm_linux_bindir}/translations"
        )
    else()
        install(FILES "${_NOVATERM_QTBASE_ZH_QM}"
            DESTINATION "${CMAKE_INSTALL_BINDIR}/translations"
        )
    endif()
endif()

# ── 按目标平台选择 CPack 生成器 ─────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CPACK_GENERATOR "DEB;ZIP")
    set(CPACK_SOURCE_GENERATOR "TGZ")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(CPACK_GENERATOR "NSIS;ZIP")
    set(CPACK_SOURCE_GENERATOR "ZIP")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(CPACK_GENERATOR "DragNDrop;ZIP")
    set(CPACK_SOURCE_GENERATOR "TGZ")
endif()

message(STATUS "CPack generators: ${CPACK_GENERATOR}")
message(STATUS "CPack file name: ${CPACK_PACKAGE_FILE_NAME}")

# ── Linux DEB 专属配置 ────────────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "li ji")
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    # 架构名按 Debian 规范：x86_64 → amd64，arm64 → arm64
    if(_pkg_arch STREQUAL "x64")
        set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
    else()
        set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${_pkg_arch}")
    endif()

    # 根据主程序、私有 Qt 库和插件的实际 ELF 依赖生成 Depends，避免与手工
    # 依赖列表重复，版本下限也由构建环境中的 Debian shlibs 数据准确给出。
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
endif()

# ── Windows NSIS 专属配置 ─────────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # 安装界面显示名（区别于内部包名 novaterm）
    set(CPACK_NSIS_PACKAGE_NAME "NovaTerm ${PROJECT_VERSION}")
    set(CPACK_NSIS_DISPLAY_NAME "NovaTerm")
    set(CPACK_NSIS_URL_INFO_ABOUT "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_NSIS_CONTACT "li ji")
    set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
    # 允许用户选择安装目录，并在开始菜单创建快捷方式
    set(CPACK_NSIS_MODIFY_PATH OFF)
    set(CPACK_NSIS_CREATE_ICONS_EXTRA
        "CreateShortCut '$SMPROGRAMS\\\\NovaTerm.lnk' '$INSTDIR\\\\${CMAKE_INSTALL_BINDIR}\\\\NovaTerm.exe'")
    set(CPACK_NSIS_DELETE_ICONS_EXTRA
        "Delete '$SMPROGRAMS\\\\NovaTerm.lnk'")
    # 安装程序图标（可选，缺失时用 NSIS 默认图标）
    if(EXISTS "${CMAKE_SOURCE_DIR}/resources/icons/app_logo.ico")
        set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/resources/icons/app_logo.ico")
        set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/resources/icons/app_logo.ico")
    endif()
endif()

# ── macOS DragNDrop (DMG) 专属配置 ────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(CPACK_DMG_VOLUME_NAME "NovaTerm")
    set(CPACK_DMG_FORMAT "UDBZ")
    set(CPACK_DRAGNDROP_INSTALLER ON)
    # 可选：自定义 DMG 布局（.DS_Store），存在时才引用
    if(EXISTS "${CMAKE_SOURCE_DIR}/cmake/DSStore")
        set(CPACK_DMG_DS_STORE "${CMAKE_SOURCE_DIR}/cmake/DSStore")
    endif()
endif()

# ── 启用 CPack ─────────────────────────────────────────────────
include(CPack)

# ── 跨平台打包辅助目标 ─────────────────────────────────────────
# cmake --build . --target package     → CPack 默认组合（DEB+ZIP / NSIS+ZIP / DMG+ZIP）
# cmake --build . --target package-zip  → 仅产 ZIP（跨平台通用，便于分发）
if(NOT TARGET package-zip)
    add_custom_target(package-zip
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target all --config $<CONFIG>
        COMMAND ${CMAKE_CPACK_COMMAND} -G ZIP -C $<CONFIG>
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Building NovaTerm and packaging as ZIP archive"
    )
endif()
