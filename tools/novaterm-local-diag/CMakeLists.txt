# MCP LocalShell 使用的固定诊断 helper；保持 console 子系统和最小依赖。
qt_add_executable(novaterm-local-diag
    main.cpp
    ${PROJECT_SOURCE_DIR}/src/session/LocalDiagnosticProtocol.h
)
set_target_properties(novaterm-local-diag PROPERTIES
    WIN32_EXECUTABLE FALSE
    MACOSX_BUNDLE FALSE
)
target_include_directories(novaterm-local-diag PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(novaterm-local-diag PRIVATE Qt6::Core)

if(APPLE)
    install(TARGETS novaterm-local-diag RUNTIME DESTINATION "NovaTerm.app/Contents/MacOS")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # SessionDirectory 从主程序同目录定位 helper，并共享应用私有 Qt 运行库。
    set_target_properties(novaterm-local-diag PROPERTIES
        INSTALL_RPATH "\$ORIGIN/../lib"
    )
    install(TARGETS novaterm-local-diag RUNTIME DESTINATION "${_novaterm_linux_bindir}")
else()
    install(TARGETS novaterm-local-diag RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
