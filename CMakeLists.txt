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
add_subdirectory(third_party/ElaWidgetTools/ElaWidgetTools)

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

# libssh
find_package(libssh 0.10 QUIET)

add_subdirectory(src)
