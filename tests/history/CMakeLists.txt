add_executable(novaterm_history_no_qt_link_check HistoryTests.cpp)
target_link_libraries(novaterm_history_no_qt_link_check PRIVATE novaterm_history)
set_target_properties(novaterm_history_no_qt_link_check PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
add_test(NAME novaterm_history_no_qt_link_check COMMAND novaterm_history_no_qt_link_check)
set_tests_properties(novaterm_history_no_qt_link_check PROPERTIES LABELS "unit;history;scrollback" TIMEOUT 30)

if(TARGET Qt6::Core)
find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
foreach(_suite Recorder Store Service)
    string(TOLOWER "${_suite}" _lower)
    qt_add_executable(novaterm_history_${_lower}_tests History${_suite}Tests.cpp)
    target_link_libraries(novaterm_history_${_lower}_tests PRIVATE novaterm_history_recording Qt6::Test)
    set_target_properties(novaterm_history_${_lower}_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME novaterm_history_${_lower}_tests COMMAND novaterm_history_${_lower}_tests
        -o "${CMAKE_CURRENT_BINARY_DIR}/History${_suite}Tests.ctest.txt,txt")
    set_tests_properties(novaterm_history_${_lower}_tests PROPERTIES LABELS "unit;history;recording;p8" TIMEOUT 30)
endforeach()
target_link_libraries(novaterm_history_store_tests PRIVATE Qt6::Sql)
qt_add_executable(novaterm_history_recording_ui_tests HistoryRecordingUiTests.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/models/HistoryCaptureModel.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/models/HistoryCaptureModel.h
    ${PROJECT_SOURCE_DIR}/src/ui/widgets/HistoryRecordingPanel.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/widgets/HistoryRecordingPanel.h)
target_link_libraries(novaterm_history_recording_ui_tests PRIVATE novaterm_history_recording Qt6::Test Qt6::Widgets ElaWidgetTools)
set_target_properties(novaterm_history_recording_ui_tests PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME novaterm_history_recording_ui_tests COMMAND novaterm_history_recording_ui_tests
    -o "${CMAKE_CURRENT_BINARY_DIR}/HistoryRecordingUiTests.ctest.txt,txt")
set_tests_properties(novaterm_history_recording_ui_tests PROPERTIES LABELS "ui;history;recording;p8" TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(NOVATERM_BUILD_BENCHMARKS)
    qt_add_executable(novaterm_history_recording_benchmark ../benchmarks/history/mcp/RecordingBenchmark.cpp)
    target_link_libraries(novaterm_history_recording_benchmark PRIVATE novaterm_history_recording Qt6::Widgets)
    if(WIN32)
        target_link_libraries(novaterm_history_recording_benchmark PRIVATE psapi)
    endif()
endif()
endif()
add_executable(novaterm_history_benchmark ../benchmarks/history/lz4/HistoryBenchmark.cpp)
target_link_libraries(novaterm_history_benchmark PRIVATE novaterm_history)
if(WIN32)
    target_link_libraries(novaterm_history_benchmark PRIVATE psapi)
endif()
set_target_properties(novaterm_history_benchmark PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_compile_definitions(novaterm_history_benchmark PRIVATE
    NOVATERM_HISTORY_BENCHMARK_BUILD_TYPE="$<CONFIG>"
    NOVATERM_HISTORY_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")
