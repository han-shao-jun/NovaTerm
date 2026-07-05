cmake_minimum_required(VERSION 3.20)
project(NovaTerm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_PREFIX_PATH /home/super/Qt/6.7.3/gcc_64/ CACHE PATH "Qt installation prefix")
    message(STATUS "This is Linux OS")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(CMAKE_PREFIX_PATH "C:\\Programs\\Qt\\6.7.3\\msvc2022_64" CACHE PATH "Qt installation prefix")
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
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Sql Svg)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Sql Svg)

message(STATUS "Using Qt ${QT_VERSION_MAJOR}.${QT_VERSION_MINOR}")

# ElaWidgetTools - built as subdirectory (relative path from WindTermQt)
add_subdirectory(third_party/ElaWidgetTools)

# QTermWidget (Qt6) depends on lxqt2-build-tools CMake modules
# Our cmake/lxqt2-build-toolsConfig.cmake points to the submodule modules directly
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
set(lxqt2-build-tools_DIR "${CMAKE_SOURCE_DIR}/cmake")

# QTermWidget - build from submodule with Qt6
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/qtermwidget/CMakeLists.txt")
    message(STATUS "Building QTermWidget from submodule (Qt6)")

    # MSVC compatibility — provide unistd.h stub for Windows
    if(MSVC)
        include_directories(BEFORE SYSTEM "${CMAKE_SOURCE_DIR}/cmake/msvc_compat")
    endif()

    add_subdirectory(third_party/qtermwidget)
    set(QTERMWIDGET_TARGET qtermwidget6)

    # Add Windows PTY stubs for linking
    if(WIN32)
        target_sources(${QTERMWIDGET_TARGET} PRIVATE
            ${CMAKE_SOURCE_DIR}/third_party/qtermwidget/lib/pty_win32_stubs.cpp)
    endif()
else()
    message(FATAL_ERROR "QTermWidget submodule not found. Run: git submodule update --init")
endif()

# Clink (chrisant996/clink v1.9.27.83514e) —— 运行时工具，不参与编译。
# 预编译的 release 目录 third_party/clink.1.9.27.83514e 在构建后事件中
# 复制到 bin/（与 NovaTerm.exe 同级）。TerminalView::startLocalShell()
# 的 cmd 类型通过 cmd.exe /k "clink.bat" inject 启动增强 cmd 会话；
# 找不到 clink.bat 时回退裸 cmd.exe。

# libssh
find_package(libssh 0.10 QUIET)

file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.h")

add_executable(NovaTerm ${SOURCES} ${CMAKE_SOURCE_DIR}/resources/resources.qrc)

target_include_directories(NovaTerm PRIVATE src)

target_link_libraries(NovaTerm PRIVATE
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Sql
        Qt${QT_VERSION_MAJOR}::Svg
        ElaWidgetTools
        ${QTERMWIDGET_TARGET}
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


set(TS_FILES
        resources/translations/novaterm_en.ts
        resources/translations/novaterm_zh_CN.ts
)

qt_add_translations(
        TARGETS NovaTerm
        TS_FILES ${TS_FILES}
)


