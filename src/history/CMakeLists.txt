# 纯后端定义供主程序和 tests/history-noqt 共用；不查找 Qt。
get_filename_component(NOVATERM_HISTORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
find_package(Threads REQUIRED)
add_library(novaterm_lz4 STATIC "${NOVATERM_HISTORY_ROOT}/third_party/lz4-1.10.0/lib/lz4.c")
target_include_directories(novaterm_lz4 PRIVATE "${NOVATERM_HISTORY_ROOT}/third_party/lz4-1.10.0/lib")
add_library(novaterm_history STATIC ScrollbackBlockCodec.cpp
    HistoryReadContext.cpp HistoryCompressionService.cpp ../core/ThreadNaming.cpp
    ../core/scrollback/ChunkedScrollback.cpp
    ../core/scrollback/TerminalLog.cpp
    ../core/scrollback/ScrollbackSnapshot.cpp
    ../core/scrollback/ScrollbackChunk.cpp)
target_compile_features(novaterm_history PUBLIC cxx_std_17)
target_include_directories(novaterm_history PUBLIC "${NOVATERM_HISTORY_ROOT}/src")
target_include_directories(novaterm_history PRIVATE "${NOVATERM_HISTORY_ROOT}/third_party/lz4-1.10.0/lib")
target_link_libraries(novaterm_history PRIVATE novaterm_lz4 PUBLIC Threads::Threads)
set_target_properties(novaterm_history novaterm_lz4 PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_compile_options(novaterm_history PUBLIC "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
target_compile_options(novaterm_history PUBLIC "$<$<CXX_COMPILER_ID:MSVC>:/Zc:enumTypes>" "$<$<CXX_COMPILER_ID:MSVC>:/permissive->")
