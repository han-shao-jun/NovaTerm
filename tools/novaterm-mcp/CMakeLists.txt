# stdio 桥接必须是 console 程序，不继承主应用的 WIN32 子系统或 GUI 依赖。
qt_add_executable(novaterm-mcp
    main.cpp StdioChannel.cpp StdioChannel.h
    ${PROJECT_SOURCE_DIR}/src/core/ThreadNaming.cpp
)
set_target_properties(novaterm-mcp PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
target_link_libraries(novaterm-mcp PRIVATE novaterm_mcp_protocol)
target_compile_definitions(novaterm-mcp PRIVATE NOVATERM_VERSION="${PROJECT_VERSION}")
if(APPLE)
    install(TARGETS novaterm-mcp RUNTIME DESTINATION "NovaTerm.app/Contents/MacOS")
    set_target_properties(novaterm-mcp PROPERTIES INSTALL_RPATH "@loader_path/../Frameworks")
else()
    install(TARGETS novaterm-mcp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
