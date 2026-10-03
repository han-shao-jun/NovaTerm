cmake_minimum_required(VERSION 3.20)

# 独立配置时不查找 Qt；嵌入根工程时不重置工程和 CTest 选项。
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(NovaTermFileTransfer LANGUAGES CXX)
    include(CTest)
    set(NOVATERM_FILETRANSFER_STANDALONE ON)
endif()

add_library(novaterm_filetransfer STATIC
    Checksum.cpp
    EngineSupport.cpp
    XyPacketCodec.cpp
    XmodemEngine.cpp
    YmodemEngine.cpp
    ZmodemCodec.cpp
    ZmodemEngine.cpp
)
target_compile_features(novaterm_filetransfer PUBLIC cxx_std_17)
target_include_directories(novaterm_filetransfer PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>"
)
set_target_properties(novaterm_filetransfer PROPERTIES
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF CXX_EXTENSIONS OFF
)
# 纯标准库目标不用 qt_add_library，以避免自动链接 Qt::Core。
if(MSVC)
    target_compile_options(novaterm_filetransfer PRIVATE /utf-8 /W4)
else()
    target_compile_options(novaterm_filetransfer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(NOVATERM_FILETRANSFER_STANDALONE AND BUILD_TESTING)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../tests/filetransfer"
                     "${CMAKE_CURRENT_BINARY_DIR}/tests")
endif()
