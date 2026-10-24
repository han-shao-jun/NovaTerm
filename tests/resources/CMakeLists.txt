find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
qt_add_executable(novaterm_resource_sharing_tests ResourceSharingTests.cpp)
target_link_libraries(novaterm_resource_sharing_tests PRIVATE novaterm_resource_sharing Qt6::Test)
set_target_properties(novaterm_resource_sharing_tests PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME novaterm_resource_sharing_tests COMMAND novaterm_resource_sharing_tests
    -o "${CMAKE_CURRENT_BINARY_DIR}/ResourceSharingTests.ctest.txt,txt")
set_tests_properties(novaterm_resource_sharing_tests PROPERTIES LABELS "resources;mcp;history-search;p8" TIMEOUT 30)
