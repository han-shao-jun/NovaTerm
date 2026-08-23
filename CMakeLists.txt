cmake_minimum_required(VERSION 3.20)
project(NovaTerm VERSION 0.1.0 LANGUAGES CXX)

# 主工程显式采用平台标准安装目录，避免依赖第三方子项目间接初始化这些变量。
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTORCC ON)

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

# 将所有运行时文件（exe、dll）输出到同一目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 将所有库文件（.so、.dylib）也输出到同一目录
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Windows 上尤其重要：DLL 和 exe 必须在一起
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Find Qt
# 注意：Qt6::Sql 已从依赖中移除——项目当前使用 MemoryProfileStore / 平台密钥链，
# 不依赖 QSqlDatabase。若将来引入 SQLite 等持久化存储，需重新加回 Sql 组件。
find_package(QT 6.8 NAMES Qt6 REQUIRED COMPONENTS Widgets Svg ShaderTools)
find_package(Qt${QT_VERSION_MAJOR} 6.8 REQUIRED COMPONENTS
    Widgets
    Svg
    ShaderTools
    LinguistTools
    SerialPort
)
qt_standard_project_setup(REQUIRES 6.8)

message(STATUS "Using Qt ${QT_VERSION_MAJOR}.${QT_VERSION_MINOR}")

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
set(CMAKE_INSTALL_PREFIX "${_novaterm_install_prefix}" CACHE PATH
    "Installation prefix" FORCE)
unset(_novaterm_install_prefix)

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
add_subdirectory(third_party/libssh-0.12.2 EXCLUDE_FROM_ALL)

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

target_include_directories(NovaTerm PRIVATE
    src
    third_party/libvterm-0.3.3/include
)

add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
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
        ElaWidgetTools
        novaterm_core
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
        #发布版除去黑窗口，无qDebu打印
        add_compile_definitions(QT_NO_DEBUG_OUTPUT)
        set_target_properties(NovaTerm PROPERTIES WIN32_EXECUTABLE TRUE)
    else ()
        #调试版保留黑窗口,有qDebug打印
    endif()
    target_compile_definitions(NovaTerm PRIVATE _WIN32 _WIN64)
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
    set(_novaterm_linux_runtime_root "${CMAKE_INSTALL_LIBDIR}/novaterm")
    set(_novaterm_linux_libdir "${_novaterm_linux_runtime_root}/lib")
    set(_novaterm_linux_pluginsdir "${_novaterm_linux_runtime_root}/plugins")
    set_target_properties(NovaTerm PROPERTIES
        OUTPUT_NAME novaterm
        INSTALL_RPATH "\$ORIGIN/../${_novaterm_linux_libdir}"
    )
    # 覆盖 ElaWidgetTools 子项目中面向独立构建的硬编码 Windows Qt RPATH。
    set_target_properties(ElaWidgetTools PROPERTIES
        INSTALL_RPATH "\$ORIGIN"
    )
endif()

# Clink 预编译 release 路径（Windows 构建主程序与测试均依赖）
if(WIN32)
    set(CLINK_SRC_DIR "${CMAKE_SOURCE_DIR}/third_party/clink.1.9.27.83514e")
endif()

# ── Windows: 复制 Clink 到构建输出，支持直接从构建目录运行 ─────
if(WIN32)
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
#  安装规则（跨平台分发）
# ═══════════════════════════════════════════════════════════════════

# Qt 官方部署 API 使用 GNUInstallDirs 的标准布局：Windows/Linux 的可执行文件
# 位于 bin/，macOS 的 .app bundle 位于安装前缀根目录。
install(
    TARGETS NovaTerm
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

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
if(WIN32 AND EXISTS "${CLINK_SRC_DIR}/clink.bat")
    install(
        FILES
            "${CLINK_SRC_DIR}/clink.bat"
            "${CLINK_SRC_DIR}/clink.lua"
            "${CLINK_SRC_DIR}/clink_x64.exe"
            "${CLINK_SRC_DIR}/clink_x86.exe"
            "${CLINK_SRC_DIR}/clink_arm64.exe"
            "${CLINK_SRC_DIR}/clink_dll_x64.dll"
            "${CLINK_SRC_DIR}/clink_dll_x86.dll"
            "${CLINK_SRC_DIR}/clink_dll_arm64.dll"
            "${CLINK_SRC_DIR}/clink.html"
        DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

# 在安装阶段收集 Qt 库、插件和平台运行时。Windows/macOS 底层分别调用
# windeployqt/macdeployqt，Linux 使用 CMake 的运行时依赖扫描。
set(_deploy_tool_options)
if(WIN32)
    list(APPEND _deploy_tool_options
        --no-system-d3d-compiler
        --no-opengl-sw
    )
endif()

qt_generate_deploy_app_script(
    TARGET NovaTerm
    OUTPUT_SCRIPT _novaterm_deploy_script
    NO_TRANSLATIONS
    DEPLOY_TOOL_OPTIONS ${_deploy_tool_options}
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # 在包含 Qt 生成的部署脚本前覆盖部署目录。qt.conf 会据此指向私有库和
    # 插件目录，主程序则通过上面的相对 INSTALL_RPATH 定位这些库。
    install(CODE "
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

# ═══════════════════════════════════════════════════════════════════
#  CPack 跨平台打包配置
#  Linux  : DEB + ZIP
#  Windows: NSIS + ZIP
#  macOS  : DragNDrop (DMG) + ZIP
# ═══════════════════════════════════════════════════════════════════

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

# .qm 已由 qt_add_translations 嵌入可执行文件，LanguageManager 也只从资源路径
# 读取翻译，因此不再重复安装一份不会被程序加载的外部翻译文件。

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

    find_package(Qt${QT_VERSION_MAJOR} 6.8 REQUIRED COMPONENTS Test)

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

    qt_add_executable(novaterm_session_tests
        tests/session/SessionTests.cpp
        src/credential/CredentialStore.cpp
        src/credential/CredentialStore.h
        src/profile/ProfileStore.cpp
        src/profile/ProfileStore.h
        src/session/SessionInputPump.cpp
        src/session/SessionInputPump.h
        src/session/SessionManager.cpp
        src/session/SessionManager.h
        src/session/SessionStore.cpp
        src/session/SessionStore.h
        src/session/SessionTypes.h
        src/session/TerminalSession.cpp
        src/session/TerminalSession.h
        src/transport/ITransport.h
    )
    target_include_directories(novaterm_session_tests PRIVATE src)
    target_compile_options(novaterm_session_tests
        PRIVATE
            "$<$<CXX_COMPILER_ID:MSVC>:/W4>"
            "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>"
    )
    target_link_libraries(novaterm_session_tests
        PRIVATE
            novaterm_core
            Qt${QT_VERSION_MAJOR}::SerialPort
            Qt${QT_VERSION_MAJOR}::Test
    )
    add_test(NAME novaterm_session_tests COMMAND novaterm_session_tests)
    set_tests_properties(novaterm_session_tests PROPERTIES
        LABELS "unit;session;p6"
        TIMEOUT 60
    )
    add_dependencies(check novaterm_session_tests)

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

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        qt_add_executable(novaterm_pty_tests
            tests/transport/PtyTransportTests.cpp
            src/transport/ITransport.h
            src/transport/LocalShellTransport.cpp
            src/transport/LocalShellTransport.h
            src/session/LocalShellProfile.cpp
            src/session/LocalShellProfile.h
            src/platform/linux/pty/PtySession.cpp
            src/platform/linux/pty/PtySession.h
        )
        target_include_directories(novaterm_pty_tests PRIVATE src)
        target_compile_options(novaterm_pty_tests
            PRIVATE -Wall -Wextra -Wpedantic
        )
        target_link_libraries(novaterm_pty_tests
            PRIVATE
                Qt${QT_VERSION_MAJOR}::Core
                Qt${QT_VERSION_MAJOR}::Test
                util
        )
        add_test(NAME novaterm_pty_tests COMMAND novaterm_pty_tests)
        set_tests_properties(novaterm_pty_tests PROPERTIES
            LABELS "integration;pty;linux"
            TIMEOUT 30
        )
        add_dependencies(check novaterm_pty_tests)
    endif()

    if(WIN32)
        qt_add_executable(novaterm_conpty_test_child
            tests/transport/ConPtyTestChild.cpp
        )

        add_executable(novaterm_conpty_residue_repro
            tests/benchmarks/ConPtyResidueRepro.cpp
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
            src/session/LocalShellProfile.cpp
            src/session/LocalShellProfile.h
            src/platform/windows/conpty/ConPtyApi.cpp
            src/platform/windows/conpty/ConPtyApi.h
            src/platform/windows/conpty/ConPtySession.cpp
            src/platform/windows/conpty/ConPtySession.h
            src/platform/windows/conpty/WinHandle.h
        )
        target_include_directories(novaterm_conpty_residue_repro PRIVATE
            src
            "${CMAKE_PREFIX_PATH}/include/QtGui/${Qt6_VERSION}/QtGui"
        )
        target_link_libraries(novaterm_conpty_residue_repro
            PRIVATE
                novaterm_core
                Qt${QT_VERSION_MAJOR}::Core
                Qt${QT_VERSION_MAJOR}::Gui
                Qt${QT_VERSION_MAJOR}::Widgets
                kernel32 user32
        )
        qt_add_shaders(novaterm_conpty_residue_repro
            novaterm_conpty_residue_repro_shaders
            PREFIX "/shaders"
            FILES
                src/renderer/shaders/terminal_texture.vert
                src/renderer/shaders/terminal_texture.frag
        )
        add_custom_command(TARGET novaterm_conpty_residue_repro POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CLINK_SRC_DIR}/clink.bat"
                "${CLINK_SRC_DIR}/clink.lua"
                "${CLINK_SRC_DIR}/clink_arm64.exe"
                "${CLINK_SRC_DIR}/clink_x64.exe"
                "${CLINK_SRC_DIR}/clink_x86.exe"
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        )

        qt_add_executable(novaterm_conpty_tests
            tests/transport/ConPtyTransportTests.cpp
            src/transport/ITransport.h
            src/transport/LocalShellTransport.cpp
            src/transport/LocalShellTransport.h
            src/session/LocalShellProfile.cpp
            src/session/LocalShellProfile.h
            src/platform/windows/conpty/ConPtyApi.cpp
            src/platform/windows/conpty/ConPtyApi.h
            src/platform/windows/conpty/ConPtySession.cpp
            src/platform/windows/conpty/ConPtySession.h
            src/platform/windows/conpty/WinHandle.h
        )
        target_include_directories(novaterm_conpty_tests PRIVATE src)
        target_compile_definitions(novaterm_conpty_tests
            PRIVATE NOVATERM_CONPTY_TESTING
        )
        target_compile_options(novaterm_conpty_tests
            PRIVATE
                "$<$<CXX_COMPILER_ID:MSVC>:/W4>"
                "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>"
        )
        target_link_libraries(novaterm_conpty_tests
            PRIVATE
                Qt${QT_VERSION_MAJOR}::Core
                Qt${QT_VERSION_MAJOR}::Test
                shell32
        )
        add_dependencies(novaterm_conpty_tests novaterm_conpty_test_child)
        add_test(NAME novaterm_conpty_tests COMMAND novaterm_conpty_tests)
        set_tests_properties(novaterm_conpty_tests PROPERTIES
            LABELS "integration;conpty"
            TIMEOUT 120
        )
        add_dependencies(check novaterm_conpty_tests)

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
            src/session/LocalShellProfile.cpp
            src/session/LocalShellProfile.h
            src/platform/windows/conpty/ConPtyApi.cpp
            src/platform/windows/conpty/ConPtyApi.h
            src/platform/windows/conpty/ConPtySession.cpp
            src/platform/windows/conpty/ConPtySession.h
            src/platform/windows/conpty/WinHandle.h
            src/transport/SshTransport.cpp
            src/transport/SshTransport.h
            src/session/SessionInputPump.cpp
            src/session/SessionInputPump.h
            src/session/TerminalSession.cpp
            src/session/TerminalSession.h
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
        add_dependencies(novaterm_terminal_session_tests
            novaterm_conpty_test_child
        )
        add_test(NAME novaterm_terminal_session_tests
                 COMMAND novaterm_terminal_session_tests)
        set_tests_properties(novaterm_terminal_session_tests
                             PROPERTIES
                                 LABELS "integration;terminal-session"
                                 TIMEOUT 120)
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

    add_executable(novaterm_residue_repro
        tests/benchmarks/ResidueRepro.cpp
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
    target_include_directories(novaterm_residue_repro PRIVATE
        src
        "${CMAKE_PREFIX_PATH}/include/QtGui/${Qt6_VERSION}/QtGui"
    )
    target_link_libraries(novaterm_residue_repro
        PRIVATE
            novaterm_core
            Qt${QT_VERSION_MAJOR}::Core
            Qt${QT_VERSION_MAJOR}::Gui
            Qt${QT_VERSION_MAJOR}::Widgets
    )
    qt_add_shaders(novaterm_residue_repro
        novaterm_residue_repro_shaders
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
