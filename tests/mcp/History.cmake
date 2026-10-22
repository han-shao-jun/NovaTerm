qt_add_executable(novaterm_history_contracts_tests ${CMAKE_CURRENT_LIST_DIR}/HistoryContractsTests.cpp)
target_link_libraries(novaterm_history_contracts_tests PRIVATE novaterm_mcp_protocol Qt6::Test)
target_compile_definitions(novaterm_history_contracts_tests PRIVATE NOVATERM_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set_target_properties(novaterm_history_contracts_tests PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME novaterm_history_contracts_tests COMMAND novaterm_history_contracts_tests
    -o "${CMAKE_CURRENT_BINARY_DIR}/HistoryContractsTests.ctest.txt,txt")
set_tests_properties(novaterm_history_contracts_tests PROPERTIES LABELS "mcp;history;p8" TIMEOUT 30)
