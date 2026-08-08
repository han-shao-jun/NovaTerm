cmake_minimum_required(VERSION 3.20)
project(NovaTerm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_PREFIX_PATH /home/super/Qt/6.8.3/gcc_64/ CACHE PATH "Qt installation prefix")
    message(STATUS "This is Linux OS")

    # RPATH: 让二进制文件在捆绑库目录中查找共享库（$ORIGIN 指向二进制所在目录）
    set(CMAKE_INSTALL_RPATH "\$ORIGIN/../lib/novaterm")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(CMAKE_PREFIX_PATH "C:\\Programs\\Qt\\6.8.3\\msvc2022_64" CACHE PATH "Qt installation prefix")
    message(STATUS "This is Windows")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    message(STATUS "This is macOS")
endif()

message(STATUS "setting qt sdk path: ${CMAKE_PREFIX_PATH}")

add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

# 将所有运行时文件（exe、dll）输出到同一目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 将所有库文件（.so、.dylib）也输出到同一目录
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Windows 上尤其重要：DLL 和 exe 必须在一起
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Find Qt
find_package(QT NAMES Qt6 REQUIRED COMPONENTS Widgets Sql Svg ShaderTools)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Sql Svg ShaderTools LinguistTools SerialPort)

message(STATUS "Using Qt ${QT_VERSION_MAJOR}.${QT_VERSION_MINOR}")

# ElaWidgetTools - built as subdirectory
add_subdirectory(third_party/ElaWidgetTools)

# libvterm — VT/ANSI 终端解析器静态库（纯 C99，无外部依赖）
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/libvterm-0.3.3/include/vterm.h")
    message(STATUS "Building libvterm from third_party/libvterm-0.3.3")
    add_subdirectory(third_party/libvterm-0.3.3)
else()
    message(FATAL_ERROR "libvterm not found at third_party/libvterm-0.3.3")
endif()

# Clink (chrisant996/clink v1.9.27.83514e) —— 运行时工具，不参与编译。
# 预编译的 release 目录 third_party/clink.1.9.27.83514e 在构建后事件中
# 复制到 bin/（与 NovaTerm.exe 同级）。TerminalView::startLocalShell()
# 的 cmd 类型通过 cmd.exe /k "clink.bat" inject 启动增强 cmd 会话；
# 找不到 clink.bat 时回退裸 cmd.exe。

# libssh — SSH 会话传输。直接以源码子目录方式构建（不再 find_package 预编译产物）。
# OpenSSL：Windows 使用 vendored 一次性预编译（scripts/build-ssh-thirdparty.bat 编译，
# 产物在 third_party/openssl-3.5.7/install）；其他平台（Linux/macOS）直接用系统
# OpenSSL（apt/dnf 安装的 libssl-dev / openssl-devel），由下方 find_package 解析。
if(WIN32)
    set(OPENSSL_INSTALL_DIR
        "${CMAKE_SOURCE_DIR}/third_party/openssl-3.5.7/install"
        CACHE PATH "OpenSSL install prefix (run scripts/build-ssh-thirdparty.bat)")
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
add_subdirectory(third_party/libssh-0.12.2)

set(NOVATERM_CORE_SOURCES
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

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp" "src/*.h")
list(FILTER SOURCES EXCLUDE REGEX "[/\\\\]src[/\\\\]core[/\\\\]")

add_executable(NovaTerm ${SOURCES} ${CMAKE_SOURCE_DIR}/resources/resources.qrc)

target_include_directories(NovaTerm PRIVATE
    src
    third_party/libvterm-0.3.3/include
)

target_link_libraries(NovaTerm PRIVATE
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Sql
        Qt${QT_VERSION_MAJOR}::Svg
        Qt${QT_VERSION_MAJOR}::SerialPort
        ElaWidgetTools
        novaterm_core
        ssh::ssh
        OpenSSL::Crypto
        OpenSSL::SSL
)

# 静态 OpenSSL 在 Windows 上的平台依赖库
if(WIN32)
    target_link_libraries(NovaTerm PRIVATE ws2_32 crypt32 user32)
endif()

if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(NovaTerm PRIVATE udev)
endif()


qt_add_shaders(NovaTerm novaterm_renderer_shaders
    PREFIX
        "/shaders"
    FILES
        src/renderer/shaders/terminal_texture.vert
        src/renderer/shaders/terminal_texture.frag
)

if(WIN32)
    if(CMAKE_BUILD_TYPE MATCHES "Release")
        #发布版除去黑窗口，无qDebu打印
        add_compile_definitions(QT_NO_DEBUG_OUTPUT)
        set_target_properties(NovaTerm PROPERTIES WIN32_EXECUTABLE TRUE)
    else ()
        #调试版保留黑窗口,有qDebug打印
    endif()
    target_compile_definitions(NovaTerm PRIVATE _WIN32 _WIN64)

    # ── 将 Clink 预编译 release 复制到 exe 同级目录 ──────────────
    set(CLINK_SRC_DIR "${CMAKE_SOURCE_DIR}/third_party/clink.1.9.27.83514e")
    if(EXISTS "${CLINK_SRC_DIR}/clink.bat")
        add_custom_command(TARGET NovaTerm POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CLINK_SRC_DIR}/clink.bat"
                "${CLINK_SRC_DIR}/clink.lua"
                "${CLINK_SRC_DIR}/clink_x64.exe"
                "${CLINK_SRC_DIR}/clink_x86.exe"
                "${CLINK_SRC_DIR}/clink_arm64.exe"
                "${CLINK_SRC_DIR}/clink_dll_x64.dll"
                "${CLINK_SRC_DIR}/clink_dll_x86.dll"
                "${CLINK_SRC_DIR}/clink_dll_arm64.dll"
                "${CLINK_SRC_DIR}/clink.html"
                $<TARGET_FILE_DIR:NovaTerm>
            COMMENT "Copying Clink release files next to NovaTerm.exe"
        )
        message(STATUS "Clink release will be deployed to output dir: ${CLINK_SRC_DIR}")
    else()
        message(WARNING "Clink release not found at ${CLINK_SRC_DIR} — cmd sessions will use bare cmd.exe")
    endif()
endif()


# ═══════════════════════════════════════════════════════════════════
#  安装规则
# ═══════════════════════════════════════════════════════════════════

# 安装主二进制文件
install(TARGETS NovaTerm DESTINATION bin)

# 安装桌面入口文件和图标（Linux 桌面集成）
install(
    FILES resources/novaterm.desktop
    DESTINATION share/applications
)
install(
    FILES resources/icons/app_logo.svg
    DESTINATION share/icons/hicolor/scalable/apps
    RENAME novaterm.svg
)

# ── Linux: 捆绑 Qt 运行时库 ──────────────────────────────────────
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    # 从 Qt6::Core 导入目标中获取 Qt 库的安装目录
    if(TARGET Qt6::Core)
        get_target_property(_qt_core_location Qt6::Core LOCATION)
        if(_qt_core_location)
            cmake_path(GET _qt_core_location PARENT_PATH _qt_lib_dir)
            message(STATUS "Qt shared libraries directory: ${_qt_lib_dir}")
        endif()
    endif()

    # 如果无法从 target 获取，回退到 CMAKE_PREFIX_PATH
    if(NOT _qt_lib_dir)
        set(_qt_lib_dir "${CMAKE_PREFIX_PATH}/lib")
    endif()

    # 将 Qt 运行时 .so 安装到 lib/novaterm/（运行时由 RPATH 查找）
    install(
        DIRECTORY "${_qt_lib_dir}/"
        DESTINATION lib/novaterm
        FILES_MATCHING
        PATTERN "libQt6Core.so*"
        PATTERN "libQt6Gui.so*"
        PATTERN "libQt6Widgets.so*"
        PATTERN "libQt6Sql.so*"
        PATTERN "libQt6Svg.so*"
        PATTERN "libQt6DBus.so*"
        PATTERN "libQt6Network.so*"
        PATTERN "libQt6XcbQpa.so*"        # X11 平台插件（Linux 桌面必需）
        PATTERN "libQt6OpenGL.so*"
        PATTERN "libQt6OpenGLWidgets.so*"
        PATTERN "libicui18n.so*"
        PATTERN "libicuuc.so*"
        PATTERN "libicudata.so*"
        PATTERN "libxcb*.so*"              # XCB 平台插件依赖
    )

    # 安装 Qt 平台插件（libqxcb.so）
    if(EXISTS "${_qt_lib_dir}/../plugins/platforms")
        install(
            DIRECTORY "${_qt_lib_dir}/../plugins/platforms"
            DESTINATION lib/novaterm/plugins
            FILES_MATCHING
            PATTERN "libqxcb.so"
        )
    endif()
endif()

# ═══════════════════════════════════════════════════════════════════
#  CPack DEB / RPM 打包配置
# ═══════════════════════════════════════════════════════════════════

set(CPACK_PACKAGE_NAME "novaterm")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-linux-amd64")
set(CPACK_PACKAGE_VENDOR "NovaTerm Project")
set(CPACK_PACKAGE_CONTACT "li ji")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/han-shao-jun/NovaTerm")

set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "NovaTerm - A modern terminal emulator")
set(CPACK_PACKAGE_DESCRIPTION
    "NovaTerm is a feature-rich terminal emulator built with Qt6 and libvterm.
It supports local and remote terminal sessions with SSH capabilities,
VT/ANSI terminal emulation, and a modern UI powered by ElaWidgetTools."
)

# ── DEB 专属配置 ─────────────────────────────────────────────
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(CPACK_GENERATOR "DEB")

    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "li ji")
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")

    # 声明系统级依赖（用户需自行安装或系统已有）
    set(CPACK_DEBIAN_PACKAGE_DEPENDS
        "libc6 (>= 2.34), libstdc++6 (>= 12), libgcc-s1 (>= 12), libgl1, libegl1, libfontconfig1, libfreetype6, libxkbcommon0, libdbus-1-3"
    )

    # 自动检测其他共享库依赖（生成 ${shlibs:Depends}）
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBDEPS ON)

    # 安装后执行 ldconfig 更新库缓存（避免 dpkg 警告）
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_BINARY_DIR}/postinst"
    )

    # 生成 postinst 维护脚本
    file(WRITE "${CMAKE_BINARY_DIR}/postinst"
        "#!/bin/sh\nset -e\n# Update shared library cache\nldconfig || true\n"
    )
endif()

# ── 打包目标：make package 或 cpack ───────────────────────────
include(CPack)

# 辅助自定义目标：一步构建 DEB
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    add_custom_target(deb
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target all
        COMMAND ${CMAKE_CPACK_COMMAND} -G DEB
        COMMENT "Building NovaTerm and packaging as .deb"
    )
    message(STATUS "DEB packaging enabled — run 'make deb' or 'cmake --build . --target deb' to produce a .deb package")
endif()


set(TS_FILES
        resources/translations/novaterm_en.ts
        resources/translations/novaterm_zh_CN.ts
)

# 为 Qt6 添加翻译文件，使用翻译工具扫描代码中的字符串生成ts文件
# 然后使用翻译工具翻译ts文件，生成qm文件
qt_add_translations(
        TARGETS NovaTerm
        TS_FILES ${TS_FILES}
)

# 通过构建系统运行 `test` 目标时，始终显示测试命令及其输出；失败时保留
# 完整日志，避免 CTest 默认摘要隐藏 Qt Test 给出的断言位置和诊断信息。
set(CMAKE_CTEST_ARGUMENTS
    "--output-on-failure"
    "--verbose"
    "--progress"
)

include(CTest)
option(NOVATERM_BUILD_BENCHMARKS "Build NovaTerm performance baseline tools" ON)
option(NOVATERM_RUN_GPU_ACCEPTANCE_TESTS
    "Register hardware-dependent GPU acceptance benchmarks with CTest" OFF)

set(NOVATERM_P5_RENDERER_SOURCES
    src/renderer/font/FontManager.cpp
    src/renderer/glyph/GlyphAtlas.cpp
    src/renderer/glyph/GlyphCache.cpp
    src/renderer/glyph/GlyphRasterizer.cpp
    src/renderer/gpu/BufferBudget.cpp
    src/renderer/gpu/MaterialBatcher.cpp
    src/renderer/gpu/RendererCapabilities.cpp
    src/renderer/gpu/RowSlotMap.cpp
)

if(BUILD_TESTING)
    # 提供一个输出更完整的统一测试入口：
    #   cmake --build <build-dir> --target check
    # 多配置生成器（Visual Studio、Ninja Multi-Config）需要显式传递配置。
    set(_novaterm_ctest_config_args)
    if(CMAKE_CONFIGURATION_TYPES)
        list(APPEND _novaterm_ctest_config_args --build-config $<CONFIG>)
    endif()

    add_custom_target(check
        COMMAND ${CMAKE_CTEST_COMMAND}
            --test-dir "${CMAKE_BINARY_DIR}"
            ${_novaterm_ctest_config_args}
            --output-on-failure
            --verbose
            --progress
        USES_TERMINAL
        COMMENT "Running NovaTerm tests with detailed CTest output"
    )
endif()

if(BUILD_TESTING)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

    add_executable(novaterm_core_tests
        tests/core/TerminalCoreTests.cpp
    )
    target_link_libraries(novaterm_core_tests
        PRIVATE
            novaterm_core
            Qt${QT_VERSION_MAJOR}::Test
    )
    add_test(NAME novaterm_core_tests COMMAND novaterm_core_tests)
    set_tests_properties(novaterm_core_tests PROPERTIES
        LABELS "unit;core"
        TIMEOUT 60
    )
    add_dependencies(check novaterm_core_tests)

    add_executable(novaterm_scrollback_tests
        tests/core/ScrollbackTests.cpp
    )
    target_link_libraries(novaterm_scrollback_tests
        PRIVATE
            novaterm_core
            Qt${QT_VERSION_MAJOR}::Test
    )
    add_test(NAME novaterm_scrollback_tests COMMAND novaterm_scrollback_tests)
    set_tests_properties(novaterm_scrollback_tests PROPERTIES
        LABELS "unit;core;scrollback"
        TIMEOUT 60
    )
    add_dependencies(check novaterm_scrollback_tests)

    add_executable(novaterm_renderer_tests
        tests/renderer/RendererP3Tests.cpp
        src/renderer/RenderCommandBuffer.cpp
        src/renderer/RenderCommandBuffer.h
        src/renderer/RenderScheduler.cpp
        src/renderer/RenderScheduler.h
        src/renderer/TerminalColorScheme.cpp
        src/renderer/TerminalColorScheme.h
        src/renderer/TerminalRenderer.cpp
        src/renderer/TerminalRenderer.h
        ${NOVATERM_P5_RENDERER_SOURCES}
    )
    target_include_directories(novaterm_renderer_tests PRIVATE
        src
        "${CMAKE_PREFIX_PATH}/include/QtGui/${Qt6_VERSION}/QtGui"
    )
    target_link_libraries(novaterm_renderer_tests
        PRIVATE
            novaterm_core
            Qt${QT_VERSION_MAJOR}::Core
            Qt${QT_VERSION_MAJOR}::Gui
            Qt${QT_VERSION_MAJOR}::Widgets
            Qt${QT_VERSION_MAJOR}::Test
    )
    add_test(NAME novaterm_renderer_tests COMMAND novaterm_renderer_tests)
    set_tests_properties(novaterm_renderer_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "unit;renderer"
        TIMEOUT 60
    )
    add_dependencies(check novaterm_renderer_tests)

    add_executable(novaterm_renderer_p5_tests
        tests/renderer/RendererP5Tests.cpp
        src/session/SerialHighlightRules.cpp
        src/renderer/RenderCommandBuffer.cpp
        src/renderer/font/FontManager.cpp
        src/renderer/glyph/GlyphAtlas.cpp
        src/renderer/glyph/GlyphCache.cpp
        src/renderer/glyph/GlyphRasterizer.cpp
        src/renderer/gpu/BufferBudget.cpp
        src/renderer/gpu/MaterialBatcher.cpp
        src/renderer/gpu/RowSlotMap.cpp
    )
    target_include_directories(novaterm_renderer_p5_tests PRIVATE src)
    target_link_libraries(novaterm_renderer_p5_tests
        PRIVATE
            Qt${QT_VERSION_MAJOR}::Core
            Qt${QT_VERSION_MAJOR}::Gui
            Qt${QT_VERSION_MAJOR}::Test
    )
    add_test(NAME novaterm_renderer_p5_tests
             COMMAND novaterm_renderer_p5_tests)
    set_tests_properties(novaterm_renderer_p5_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "unit;renderer;p5"
        TIMEOUT 60
    )
    add_dependencies(check novaterm_renderer_p5_tests)

    if(WIN32)
        add_executable(novaterm_terminal_session_tests
            tests/renderer/TerminalSessionSmokeTests.cpp
            src/renderer/RenderCommandBuffer.cpp
            src/renderer/RenderCommandBuffer.h
            src/renderer/RenderScheduler.cpp
            src/renderer/RenderScheduler.h
            src/renderer/TerminalColorScheme.cpp
            src/renderer/TerminalColorScheme.h
            src/renderer/TerminalRenderer.cpp
            src/renderer/TerminalRenderer.h
            ${NOVATERM_P5_RENDERER_SOURCES}
            src/transport/ITransport.h
            src/transport/LocalShellTransport.cpp
            src/transport/LocalShellTransport.h
            src/transport/SshTransport.cpp
            src/transport/SshTransport.h
            src/session/SessionInputPump.cpp
            src/session/SessionInputPump.h
            src/service/ConfigManager.cpp
            src/service/ConfigManager.h
            src/ui/terminal/TerminalView.cpp
            src/ui/terminal/TerminalView.h
            src/ui/widgets/SshHostKeyDialog.cpp
            src/ui/widgets/SshHostKeyDialog.h
        )
        target_include_directories(novaterm_terminal_session_tests PRIVATE
            src
            "${CMAKE_PREFIX_PATH}/include/QtGui/${Qt6_VERSION}/QtGui"
        )
        target_link_libraries(novaterm_terminal_session_tests
            PRIVATE
                novaterm_core
                ElaWidgetTools
                Qt${QT_VERSION_MAJOR}::Core
                Qt${QT_VERSION_MAJOR}::Gui
                Qt${QT_VERSION_MAJOR}::Widgets
                Qt${QT_VERSION_MAJOR}::SerialPort
                Qt${QT_VERSION_MAJOR}::Test
                ssh::ssh
                OpenSSL::Crypto
                OpenSSL::SSL
        )
        if(WIN32)
            target_link_libraries(novaterm_terminal_session_tests
                PRIVATE ws2_32 crypt32 user32)
        endif()
        qt_add_shaders(novaterm_terminal_session_tests
            novaterm_terminal_session_test_shaders
            PREFIX "/shaders"
            FILES
                src/renderer/shaders/terminal_texture.vert
                src/renderer/shaders/terminal_texture.frag
        )
        add_custom_command(TARGET novaterm_terminal_session_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CLINK_SRC_DIR}/clink.bat"
                "${CLINK_SRC_DIR}/clink.lua"
                "${CLINK_SRC_DIR}/clink_x64.exe"
                "${CLINK_SRC_DIR}/clink_dll_x64.dll"
                $<TARGET_FILE_DIR:novaterm_terminal_session_tests>
        )
        add_test(NAME novaterm_terminal_session_tests
                 COMMAND novaterm_terminal_session_tests)
        set_tests_properties(novaterm_terminal_session_tests
                             PROPERTIES
                                 LABELS "integration;terminal-session"
                                 TIMEOUT 20)
        add_dependencies(check novaterm_terminal_session_tests)
    endif()

    # ── SshTransport 失败路径控制台检查（无需真实 SSH 服务器）──
    add_executable(novaterm_ssh_transport_check
        tests/transport/SshTransportFailureCheck.cpp
        src/transport/SshTransport.cpp
        src/transport/SshTransport.h
        src/transport/ITransport.h
        src/session/SessionTypes.h
    )
    target_include_directories(novaterm_ssh_transport_check PRIVATE src)
    target_link_libraries(novaterm_ssh_transport_check
        PRIVATE
            Qt${QT_VERSION_MAJOR}::Core
            Qt${QT_VERSION_MAJOR}::SerialPort
            ssh::ssh
            OpenSSL::Crypto
            OpenSSL::SSL
    )
    if(WIN32)
        target_link_libraries(novaterm_ssh_transport_check
            PRIVATE ws2_32 crypt32 user32)
    endif()
    add_test(NAME novaterm_ssh_transport_check
             COMMAND novaterm_ssh_transport_check)
    set_tests_properties(novaterm_ssh_transport_check
                         PROPERTIES
                             LABELS "transport;ssh"
                             TIMEOUT 30)
    add_dependencies(check novaterm_ssh_transport_check)
endif()

if(NOVATERM_BUILD_BENCHMARKS)
    add_executable(novaterm_renderer_p5_gpu_benchmark
        tests/benchmarks/RendererP5GpuBenchmark.cpp
        src/renderer/RenderCommandBuffer.cpp
        src/renderer/RenderScheduler.cpp
        src/renderer/TerminalColorScheme.cpp
        src/renderer/TerminalRenderer.cpp
        ${NOVATERM_P5_RENDERER_SOURCES}
    )
    target_include_directories(novaterm_renderer_p5_gpu_benchmark PRIVATE
        src
        "${CMAKE_PREFIX_PATH}/include/QtGui/${Qt6_VERSION}/QtGui"
    )
    target_link_libraries(novaterm_renderer_p5_gpu_benchmark PRIVATE
        novaterm_core
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui
        Qt${QT_VERSION_MAJOR}::Widgets
    )
    qt_add_shaders(novaterm_renderer_p5_gpu_benchmark
        novaterm_renderer_p5_gpu_benchmark_shaders
        PREFIX "/shaders"
        FILES
            src/renderer/shaders/terminal_texture.vert
            src/renderer/shaders/terminal_texture.frag
    )
    if(BUILD_TESTING AND NOVATERM_RUN_GPU_ACCEPTANCE_TESTS)
        add_test(NAME novaterm_renderer_p5_gpu_acceptance
            COMMAND novaterm_renderer_p5_gpu_benchmark
                --duration-ms 5000
                --refresh-rate 60
                --scrollback-limit 100000
                --prefill-lines 100000
        )
        set_tests_properties(novaterm_renderer_p5_gpu_acceptance
            PROPERTIES
                LABELS "integration;renderer;gpu;benchmark"
                RUN_SERIAL TRUE
                TIMEOUT 180
        )
        add_dependencies(check novaterm_renderer_p5_gpu_benchmark)
    endif()

    add_executable(novaterm_renderer_p3_gpu_benchmark
        tests/benchmarks/RendererP3GpuBenchmark.cpp
        src/renderer/RenderCommandBuffer.cpp
        src/renderer/RenderCommandBuffer.h
        src/renderer/RenderScheduler.cpp
        src/renderer/RenderScheduler.h
        src/renderer/TerminalColorScheme.cpp
        src/renderer/TerminalColorScheme.h
        src/renderer/TerminalRenderer.cpp
        src/renderer/TerminalRenderer.h
        ${NOVATERM_P5_RENDERER_SOURCES}
    )
    target_include_directories(novaterm_renderer_p3_gpu_benchmark PRIVATE
        src
        "${CMAKE_PREFIX_PATH}/include/QtGui/${Qt6_VERSION}/QtGui"
    )
    target_link_libraries(novaterm_renderer_p3_gpu_benchmark
        PRIVATE
            novaterm_core
            Qt${QT_VERSION_MAJOR}::Core
            Qt${QT_VERSION_MAJOR}::Gui
            Qt${QT_VERSION_MAJOR}::Widgets
    )
    qt_add_shaders(novaterm_renderer_p3_gpu_benchmark
        novaterm_renderer_p3_gpu_benchmark_shaders
        PREFIX "/shaders"
        FILES
            src/renderer/shaders/terminal_texture.vert
            src/renderer/shaders/terminal_texture.frag
    )

    add_executable(novaterm_renderer_p3_benchmark
        tests/benchmarks/RendererP3Benchmark.cpp
        src/renderer/RenderCommandBuffer.cpp
        src/renderer/RenderCommandBuffer.h
    )
    target_include_directories(novaterm_renderer_p3_benchmark PRIVATE src)
    target_link_libraries(novaterm_renderer_p3_benchmark
        PRIVATE
            novaterm_core
            Qt${QT_VERSION_MAJOR}::Core
            Qt${QT_VERSION_MAJOR}::Gui
    )

    add_executable(novaterm_core_benchmark
        tests/benchmarks/CoreBenchmark.cpp
    )
    target_link_libraries(novaterm_core_benchmark PRIVATE novaterm_core)

    add_executable(novaterm_scrollback_benchmark
        tests/benchmarks/ScrollbackBenchmark.cpp
    )
    target_link_libraries(novaterm_scrollback_benchmark PRIVATE novaterm_core)

    add_custom_target(scrollback_benchmark_matrix
        COMMAND $<TARGET_FILE:novaterm_scrollback_benchmark>
            --matrix
            --matrix-lines 1000,10000,100000,1000000
            --columns 80
            --chunk-lines 1024
            --max-bytes 268435456
        DEPENDS novaterm_scrollback_benchmark
        USES_TERMINAL
        COMMENT "Running isolated NovaTerm P4 scrollback size/RSS/latency matrix"
    )

    # Benchmark 使用自己的分区性能报告，不经过 CTest。参数对应 P2/P4
    # 设计文档中的 20 MiB Parser 与 10 万行 Scrollback 基线。
    add_custom_target(benchmarks
        COMMAND $<TARGET_FILE:novaterm_renderer_p3_benchmark>
            --iterations 20000
        COMMAND $<TARGET_FILE:novaterm_core_benchmark>
            --bytes 20971520
            --lines 100000
        COMMAND $<TARGET_FILE:novaterm_scrollback_benchmark>
            --lines 100000
            --columns 80
            --chunk-lines 1024
            --max-bytes 268435456
        DEPENDS
            novaterm_core_benchmark
            novaterm_renderer_p3_benchmark
            novaterm_scrollback_benchmark
        USES_TERMINAL
        COMMENT "Running NovaTerm P2/P4 performance benchmarks"
    )
endif()

# 安装 .qm 翻译文件到 bin 同级目录
install(
    DIRECTORY "${CMAKE_BINARY_DIR}/translations"
    DESTINATION bin
    FILES_MATCHING
    PATTERN "*.qm"
)
