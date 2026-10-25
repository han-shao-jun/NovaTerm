add_executable(novaterm_history_no_qt_link_check HistoryTests.cpp)
target_link_libraries(novaterm_history_no_qt_link_check PRIVATE novaterm_history)
set_target_properties(novaterm_history_no_qt_link_check PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
add_test(NAME novaterm_history_no_qt_link_check COMMAND novaterm_history_no_qt_link_check)
set_tests_properties(novaterm_history_no_qt_link_check PROPERTIES LABELS "unit;history;scrollback" TIMEOUT 30)

add_executable(novaterm_history_benchmark ../benchmarks/history/lz4/HistoryBenchmark.cpp)
target_link_libraries(novaterm_history_benchmark PRIVATE novaterm_history)
if(WIN32)
    target_link_libraries(novaterm_history_benchmark PRIVATE psapi)
endif()
set_target_properties(novaterm_history_benchmark PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_compile_definitions(novaterm_history_benchmark PRIVATE
    NOVATERM_HISTORY_BENCHMARK_BUILD_TYPE="$<CONFIG>"
    NOVATERM_HISTORY_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")
