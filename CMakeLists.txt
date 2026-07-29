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
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Sql Svg ShaderTools LinguistTools)

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

# libssh
find_package(libssh 0.10 QUIET)

set(NOVATERM_CORE_SOURCES
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
    "${CMAKE_PREFIX_PATH}/include/QtGui/${Qt6_VERSION}/QtGui"
)

target_link_libraries(NovaTerm PRIVATE
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Sql
        Qt${QT_VERSION_MAJOR}::Svg
        ElaWidgetTools
        novaterm_core
)

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
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/example/novaterm")

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

include(CTest)
option(NOVATERM_BUILD_BENCHMARKS "Build NovaTerm performance baseline tools" ON)

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
endif()

if(NOVATERM_BUILD_BENCHMARKS)
    add_executable(novaterm_core_benchmark
        benchmarks/CoreBenchmark.cpp
    )
    target_link_libraries(novaterm_core_benchmark PRIVATE novaterm_core)
endif()

# 安装 .qm 翻译文件到 bin 同级目录
install(
    DIRECTORY "${CMAKE_BINARY_DIR}/translations"
    DESTINATION bin
    FILES_MATCHING
    PATTERN "*.qm"
)
