# NovaTerm 测试、人工检查与性能基准目标。
# 本文件由根 CMakeLists.txt 通过 add_subdirectory(tests) 引入。

if(WIN32)
    set(_novaterm_clink_src_dir
        "${PROJECT_SOURCE_DIR}/third_party/clink.1.9.27.83514e"
    )
endif()

option(NOVATERM_BUILD_BENCHMARKS "Build NovaTerm performance baseline tools" ON)
option(NOVATERM_RUN_GPU_ACCEPTANCE_TESTS
    "Register hardware-dependent GPU acceptance benchmarks with CTest" OFF)

set(NOVATERM_P5_RENDERER_SOURCES
    ${PROJECT_SOURCE_DIR}/src/renderer/font/FontManager.cpp
    ${PROJECT_SOURCE_DIR}/src/renderer/glyph/GlyphAtlas.cpp
    ${PROJECT_SOURCE_DIR}/src/renderer/glyph/GlyphCache.cpp
    ${PROJECT_SOURCE_DIR}/src/renderer/glyph/GlyphRasterizer.cpp
    ${PROJECT_SOURCE_DIR}/src/renderer/gpu/BufferBudget.cpp
    ${PROJECT_SOURCE_DIR}/src/renderer/gpu/MaterialBatcher.cpp
    ${PROJECT_SOURCE_DIR}/src/renderer/gpu/RendererCapabilities.cpp
    ${PROJECT_SOURCE_DIR}/src/renderer/gpu/RowSlotMap.cpp
)

if(BUILD_TESTING)
    # 提供一个输出更完整的统一测试入口：
    #   cmake --build <build-dir> --target check
    # 多配置生成器（Visual Studio、Ninja Multi-Config）需要显式传递配置。
    set(_novaterm_ctest_config_args)
    if(CMAKE_CONFIGURATION_TYPES)
        list(APPEND _novaterm_ctest_config_args --build-config $<CONFIG>)
    endif()

    add_custom_target(check
        COMMAND ${CMAKE_CTEST_COMMAND}
            --test-dir "${CMAKE_BINARY_DIR}"
            ${_novaterm_ctest_config_args}
            --output-on-failure
            --verbose
            --progress
        USES_TERMINAL
        COMMENT "Running NovaTerm tests with detailed CTest output"
    )

    find_package(Qt${QT_VERSION_MAJOR} 6.8 REQUIRED COMPONENTS Test)

    add_executable(novaterm_local_executor_test_child
        mcp/LocalExecutorTestChild.cpp
    )
    set_target_properties(novaterm_local_executor_test_child PROPERTIES
        WIN32_EXECUTABLE FALSE
    )

    qt_add_executable(novaterm_mcp_tests
        mcp/McpTests.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/widgets/McpSettingsDialog.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/widgets/McpSettingsDialog.h
        ${PROJECT_SOURCE_DIR}/src/session/TerminalSession.cpp
        ${PROJECT_SOURCE_DIR}/src/session/TerminalSession.h
        ${PROJECT_SOURCE_DIR}/src/session/SessionInputPump.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionInputPump.h
        ${PROJECT_SOURCE_DIR}/src/session/SessionInputArbiter.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionInputArbiter.h
        ${PROJECT_SOURCE_DIR}/src/session/InteractiveCommandProfile.cpp
        ${PROJECT_SOURCE_DIR}/src/session/InteractiveCommandProfile.h
        ${PROJECT_SOURCE_DIR}/src/session/InteractiveStreamFramer.cpp
        ${PROJECT_SOURCE_DIR}/src/session/InteractiveStreamFramer.h
        ${PROJECT_SOURCE_DIR}/src/session/SessionCommandCoordinator.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionCommandCoordinator.h
        ${PROJECT_SOURCE_DIR}/src/session/InteractiveSessionCommandExecutor.cpp
        ${PROJECT_SOURCE_DIR}/src/session/InteractiveSessionCommandExecutor.h
        ${PROJECT_SOURCE_DIR}/src/credential/CredentialStore.cpp
        ${PROJECT_SOURCE_DIR}/src/transport/SshTransport.cpp
        ${PROJECT_SOURCE_DIR}/src/transport/SshTransport.h
        ${PROJECT_SOURCE_DIR}/src/transport/SshMonitorProtocol.cpp
        ${PROJECT_SOURCE_DIR}/src/transport/ITransport.h
    )
    target_link_libraries(novaterm_mcp_tests PRIVATE novaterm_mcp_runtime Qt6::Test Qt6::Widgets ElaWidgetTools
        ssh::ssh OpenSSL::Crypto OpenSSL::SSL ${NOVATERM_SECRET_SERVICE_LIBS})
    if(WIN32)
        target_link_libraries(novaterm_mcp_tests PRIVATE ws2_32 crypt32 advapi32)
    endif()
    target_compile_definitions(novaterm_mcp_tests PRIVATE
        NOVATERM_MCP_BRIDGE="$<TARGET_FILE:novaterm-mcp>"
        NOVATERM_LOCAL_DIAG="$<TARGET_FILE:novaterm-local-diag>"
        NOVATERM_LOCAL_EXECUTOR_TEST_CHILD="$<TARGET_FILE:novaterm_local_executor_test_child>"
    )
    add_dependencies(novaterm_mcp_tests novaterm-mcp novaterm-local-diag
        novaterm_local_executor_test_child)
    set_target_properties(novaterm_mcp_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME novaterm_mcp_tests COMMAND novaterm_mcp_tests)
    set_tests_properties(novaterm_mcp_tests PROPERTIES LABELS "mcp;p8" TIMEOUT 120
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_dependencies(check novaterm_mcp_tests)

    add_executable(novaterm_core_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/core/TerminalCoreTests.cpp
    )
    target_link_libraries(novaterm_core_tests
        PRIVATE
            novaterm_core
            Qt${QT_VERSION_MAJOR}::Test
    )
    # KeyMapper.h 在 VTAdapter 边界暴露 VTermKey/VTermModifier；该测试直接
    # 验证到 libvterm 键码的映射，故需要 libvterm 头目录（仅测试目标可见，
    # 不改变生产分层）。
    target_include_directories(novaterm_core_tests
        PRIVATE ${PROJECT_SOURCE_DIR}/third_party/libvterm-0.3.3/include)
    add_test(NAME novaterm_core_tests COMMAND novaterm_core_tests)
    set_tests_properties(novaterm_core_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        LABELS "unit;core"
        TIMEOUT 60
    )
    add_dependencies(check novaterm_core_tests)

    add_executable(novaterm_scrollback_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ScrollbackTests.cpp
    )
    target_link_libraries(novaterm_scrollback_tests
        PRIVATE
            novaterm_core
            Qt${QT_VERSION_MAJOR}::Test
    )
    add_test(NAME novaterm_scrollback_tests COMMAND novaterm_scrollback_tests)
    set_tests_properties(novaterm_scrollback_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        LABELS "unit;core;scrollback"
        TIMEOUT 60
    )
    add_dependencies(check novaterm_scrollback_tests)

    qt_add_executable(novaterm_session_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/session/SessionTests.cpp
        ${PROJECT_SOURCE_DIR}/src/credential/CredentialStore.cpp
        ${PROJECT_SOURCE_DIR}/src/credential/CredentialStore.h
        ${PROJECT_SOURCE_DIR}/src/profile/ProfileStore.cpp
        ${PROJECT_SOURCE_DIR}/src/profile/ProfileStore.h
        ${PROJECT_SOURCE_DIR}/src/session/SessionInputPump.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionInputPump.h
        ${PROJECT_SOURCE_DIR}/src/session/SessionInputArbiter.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionInputArbiter.h
        ${PROJECT_SOURCE_DIR}/src/session/InteractiveCommandProfile.cpp
        ${PROJECT_SOURCE_DIR}/src/session/InteractiveCommandProfile.h
        ${PROJECT_SOURCE_DIR}/src/session/InteractiveStreamFramer.cpp
        ${PROJECT_SOURCE_DIR}/src/session/InteractiveStreamFramer.h
        ${PROJECT_SOURCE_DIR}/src/session/SessionCommandCoordinator.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionCommandCoordinator.h
        ${PROJECT_SOURCE_DIR}/src/session/InteractiveSessionCommandExecutor.cpp
        ${PROJECT_SOURCE_DIR}/src/session/InteractiveSessionCommandExecutor.h
        ${PROJECT_SOURCE_DIR}/src/session/SessionStore.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionStore.h
        ${PROJECT_SOURCE_DIR}/src/session/ISessionCommandExecutor.h
        ${PROJECT_SOURCE_DIR}/src/session/CommandPlatformProfile.cpp
        ${PROJECT_SOURCE_DIR}/src/session/CommandPlatformProfile.h
        ${PROJECT_SOURCE_DIR}/src/session/SessionCommandFacade.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionCommandFacade.h
        ${PROJECT_SOURCE_DIR}/src/session/SessionTypes.h
        ${PROJECT_SOURCE_DIR}/src/session/TerminalSession.cpp
        ${PROJECT_SOURCE_DIR}/src/session/TerminalSession.h
        ${PROJECT_SOURCE_DIR}/src/transport/ITransport.h
    )
    target_include_directories(novaterm_session_tests
        PRIVATE ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(novaterm_session_tests
        PRIVATE
            novaterm_core
            Qt${QT_VERSION_MAJOR}::SerialPort
            Qt${QT_VERSION_MAJOR}::Test
            ${NOVATERM_SECRET_SERVICE_LIBS}
    )
    add_test(NAME novaterm_session_tests COMMAND novaterm_session_tests)
    set_tests_properties(novaterm_session_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        LABELS "unit;session;p6"
        TIMEOUT 60
    )
    add_dependencies(check novaterm_session_tests)

    add_executable(novaterm_renderer_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/renderer/RendererP3Tests.cpp
        ${PROJECT_SOURCE_DIR}/src/service/TerminalSchemeStore.cpp
        ${PROJECT_SOURCE_DIR}/src/service/TerminalSchemeStore.h
        ${PROJECT_SOURCE_DIR}/src/renderer/RenderCommandBuffer.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/RenderCommandBuffer.h
        ${PROJECT_SOURCE_DIR}/src/renderer/RenderScheduler.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/RenderScheduler.h
        ${PROJECT_SOURCE_DIR}/src/renderer/TerminalColorScheme.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/TerminalColorScheme.h
        ${PROJECT_SOURCE_DIR}/src/renderer/TerminalRenderer.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/TerminalRenderer.h
        ${NOVATERM_P5_RENDERER_SOURCES}
    )
    novaterm_add_scheme_resources(novaterm_renderer_tests)
    target_include_directories(novaterm_renderer_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        "${CMAKE_PREFIX_PATH}/include/QtGui/${Qt6_VERSION}/QtGui"
    )
    target_link_libraries(novaterm_renderer_tests
        PRIVATE
            novaterm_core
            Qt${QT_VERSION_MAJOR}::Core
            Qt${QT_VERSION_MAJOR}::Gui
            Qt${QT_VERSION_MAJOR}::Widgets
            Qt${QT_VERSION_MAJOR}::Test
    )
    add_test(NAME novaterm_renderer_tests COMMAND novaterm_renderer_tests)
    set_tests_properties(novaterm_renderer_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "unit;renderer"
        TIMEOUT 60
    )
    add_dependencies(check novaterm_renderer_tests)

    add_executable(novaterm_renderer_p5_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/renderer/RendererP5Tests.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SerialHighlightRules.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/RenderCommandBuffer.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/font/FontManager.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/glyph/GlyphAtlas.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/glyph/GlyphCache.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/glyph/GlyphRasterizer.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/gpu/BufferBudget.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/gpu/MaterialBatcher.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/gpu/RowSlotMap.cpp
    )
    target_include_directories(novaterm_renderer_p5_tests
        PRIVATE ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(novaterm_renderer_p5_tests
        PRIVATE
            # RowBlockDamageTracker 的回归测试改用 TerminalCore 快照提供块指纹，
            # 故本目标不再是"只编译 renderer 源文件"的纯头文件消费者。
            novaterm_core
            Qt${QT_VERSION_MAJOR}::Core
            Qt${QT_VERSION_MAJOR}::Gui
            Qt${QT_VERSION_MAJOR}::Test
    )
    add_test(NAME novaterm_renderer_p5_tests
             COMMAND novaterm_renderer_p5_tests)
    set_tests_properties(novaterm_renderer_p5_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "unit;renderer;p5"
        TIMEOUT 60
    )
    add_dependencies(check novaterm_renderer_p5_tests)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        qt_add_executable(novaterm_pty_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/transport/PtyTransportTests.cpp
            ${PROJECT_SOURCE_DIR}/src/transport/ITransport.h
            ${PROJECT_SOURCE_DIR}/src/transport/LocalShellTransport.cpp
            ${PROJECT_SOURCE_DIR}/src/transport/LocalShellTransport.h
            ${PROJECT_SOURCE_DIR}/src/session/LocalShellProfile.cpp
            ${PROJECT_SOURCE_DIR}/src/session/LocalShellProfile.h
            ${PROJECT_SOURCE_DIR}/src/platform/linux/pty/PtySession.cpp
            ${PROJECT_SOURCE_DIR}/src/platform/linux/pty/PtySession.h
        )
        target_include_directories(novaterm_pty_tests
            PRIVATE ${PROJECT_SOURCE_DIR}/src
        )
        target_link_libraries(novaterm_pty_tests
            PRIVATE
                Qt${QT_VERSION_MAJOR}::Core
                Qt${QT_VERSION_MAJOR}::Test
                util
        )
        add_test(NAME novaterm_pty_tests COMMAND novaterm_pty_tests)
        set_tests_properties(novaterm_pty_tests PROPERTIES
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            LABELS "integration;pty;linux"
            TIMEOUT 30
        )
        add_dependencies(check novaterm_pty_tests)
    endif()

    if(WIN32)
        qt_add_executable(novaterm_conpty_test_child
            ${CMAKE_CURRENT_SOURCE_DIR}/transport/ConPtyTestChild.cpp
        )

        add_executable(novaterm_conpty_residue_repro
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/ConPtyResidueRepro.cpp
            ${PROJECT_SOURCE_DIR}/src/renderer/RenderCommandBuffer.cpp
            ${PROJECT_SOURCE_DIR}/src/renderer/RenderCommandBuffer.h
            ${PROJECT_SOURCE_DIR}/src/renderer/RenderScheduler.cpp
            ${PROJECT_SOURCE_DIR}/src/renderer/RenderScheduler.h
            ${PROJECT_SOURCE_DIR}/src/renderer/TerminalColorScheme.cpp
            ${PROJECT_SOURCE_DIR}/src/renderer/TerminalColorScheme.h
            ${PROJECT_SOURCE_DIR}/src/renderer/TerminalRenderer.cpp
            ${PROJECT_SOURCE_DIR}/src/renderer/TerminalRenderer.h
            ${NOVATERM_P5_RENDERER_SOURCES}
            ${PROJECT_SOURCE_DIR}/src/transport/ITransport.h
            ${PROJECT_SOURCE_DIR}/src/transport/LocalShellTransport.cpp
            ${PROJECT_SOURCE_DIR}/src/transport/LocalShellTransport.h
            ${PROJECT_SOURCE_DIR}/src/session/LocalShellProfile.cpp
            ${PROJECT_SOURCE_DIR}/src/session/LocalShellProfile.h
            ${PROJECT_SOURCE_DIR}/src/platform/windows/conpty/ConPtyApi.cpp
            ${PROJECT_SOURCE_DIR}/src/platform/windows/conpty/ConPtyApi.h
            ${PROJECT_SOURCE_DIR}/src/platform/windows/conpty/ConPtySession.cpp
            ${PROJECT_SOURCE_DIR}/src/platform/windows/conpty/ConPtySession.h
            ${PROJECT_SOURCE_DIR}/src/platform/windows/conpty/WinHandle.h
        )
        target_include_directories(novaterm_conpty_residue_repro PRIVATE
            ${PROJECT_SOURCE_DIR}/src
            "${CMAKE_PREFIX_PATH}/include/QtGui/${Qt6_VERSION}/QtGui"
        )
        target_link_libraries(novaterm_conpty_residue_repro
            PRIVATE
                novaterm_core
                Qt${QT_VERSION_MAJOR}::Core
                Qt${QT_VERSION_MAJOR}::Gui
                Qt${QT_VERSION_MAJOR}::Widgets
                kernel32 user32
        )
        qt_add_shaders(novaterm_conpty_residue_repro
            novaterm_conpty_residue_repro_shaders
            BASE ${PROJECT_SOURCE_DIR}
            PREFIX "/shaders"
            FILES
                ${PROJECT_SOURCE_DIR}/src/renderer/shaders/terminal_texture.vert
                ${PROJECT_SOURCE_DIR}/src/renderer/shaders/terminal_texture.frag
        )
        add_custom_command(TARGET novaterm_conpty_residue_repro POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_novaterm_clink_src_dir}/clink.bat"
                "${_novaterm_clink_src_dir}/clink.lua"
                "${_novaterm_clink_src_dir}/clink_arm64.exe"
                "${_novaterm_clink_src_dir}/clink_x64.exe"
                "${_novaterm_clink_src_dir}/clink_x86.exe"
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        )

        qt_add_executable(novaterm_conpty_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/transport/ConPtyTransportTests.cpp
            # ConPtySession.cpp 为四个工作线程设置线程名；本目标直接编译生产
            # 源文件、不链接 novaterm_core，故显式带上该实现。
            ${PROJECT_SOURCE_DIR}/src/core/ThreadNaming.cpp
            ${PROJECT_SOURCE_DIR}/src/transport/ITransport.h
            ${PROJECT_SOURCE_DIR}/src/transport/LocalShellTransport.cpp
            ${PROJECT_SOURCE_DIR}/src/transport/LocalShellTransport.h
            ${PROJECT_SOURCE_DIR}/src/session/LocalShellProfile.cpp
            ${PROJECT_SOURCE_DIR}/src/session/LocalShellProfile.h
            ${PROJECT_SOURCE_DIR}/src/platform/windows/conpty/ConPtyApi.cpp
            ${PROJECT_SOURCE_DIR}/src/platform/windows/conpty/ConPtyApi.h
            ${PROJECT_SOURCE_DIR}/src/platform/windows/conpty/ConPtySession.cpp
            ${PROJECT_SOURCE_DIR}/src/platform/windows/conpty/ConPtySession.h
            ${PROJECT_SOURCE_DIR}/src/platform/windows/conpty/WinHandle.h
        )
        target_include_directories(novaterm_conpty_tests
            PRIVATE ${PROJECT_SOURCE_DIR}/src
        )
        target_compile_definitions(novaterm_conpty_tests
            PRIVATE NOVATERM_CONPTY_TESTING
        )
        target_link_libraries(novaterm_conpty_tests
            PRIVATE
                Qt${QT_VERSION_MAJOR}::Core
                Qt${QT_VERSION_MAJOR}::Test
                shell32
        )
        add_dependencies(novaterm_conpty_tests novaterm_conpty_test_child)
        add_test(NAME novaterm_conpty_tests COMMAND novaterm_conpty_tests)
        set_tests_properties(novaterm_conpty_tests PROPERTIES
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            LABELS "integration;conpty"
            TIMEOUT 120
        )
        add_dependencies(check novaterm_conpty_tests)

        add_executable(novaterm_terminal_session_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/renderer/TerminalSessionSmokeTests.cpp
            ${PROJECT_SOURCE_DIR}/src/service/TerminalSchemeStore.cpp
            ${PROJECT_SOURCE_DIR}/src/service/TerminalSchemeStore.h
            ${PROJECT_SOURCE_DIR}/src/renderer/RenderCommandBuffer.cpp
            ${PROJECT_SOURCE_DIR}/src/renderer/RenderCommandBuffer.h
            ${PROJECT_SOURCE_DIR}/src/renderer/RenderScheduler.cpp
            ${PROJECT_SOURCE_DIR}/src/renderer/RenderScheduler.h
            ${PROJECT_SOURCE_DIR}/src/renderer/TerminalColorScheme.cpp
            ${PROJECT_SOURCE_DIR}/src/renderer/TerminalColorScheme.h
            ${PROJECT_SOURCE_DIR}/src/renderer/TerminalRenderer.cpp
            ${PROJECT_SOURCE_DIR}/src/renderer/TerminalRenderer.h
            ${NOVATERM_P5_RENDERER_SOURCES}
            ${PROJECT_SOURCE_DIR}/src/transport/ITransport.h
            ${PROJECT_SOURCE_DIR}/src/transport/LocalShellTransport.cpp
            ${PROJECT_SOURCE_DIR}/src/transport/LocalShellTransport.h
            ${PROJECT_SOURCE_DIR}/src/session/LocalShellProfile.cpp
            ${PROJECT_SOURCE_DIR}/src/session/LocalShellProfile.h
            ${PROJECT_SOURCE_DIR}/src/platform/windows/conpty/ConPtyApi.cpp
            ${PROJECT_SOURCE_DIR}/src/platform/windows/conpty/ConPtyApi.h
            ${PROJECT_SOURCE_DIR}/src/platform/windows/conpty/ConPtySession.cpp
            ${PROJECT_SOURCE_DIR}/src/platform/windows/conpty/ConPtySession.h
            ${PROJECT_SOURCE_DIR}/src/platform/windows/conpty/WinHandle.h
            ${PROJECT_SOURCE_DIR}/src/transport/SshTransport.cpp
            ${PROJECT_SOURCE_DIR}/src/transport/SshTransport.h
            ${PROJECT_SOURCE_DIR}/src/transport/SshMonitorProtocol.cpp
            ${PROJECT_SOURCE_DIR}/src/transport/SshMonitorProtocol.h
            ${PROJECT_SOURCE_DIR}/src/session/SessionInputPump.cpp
            ${PROJECT_SOURCE_DIR}/src/session/SessionInputPump.h
            ${PROJECT_SOURCE_DIR}/src/session/SessionInputArbiter.cpp
            ${PROJECT_SOURCE_DIR}/src/session/SessionInputArbiter.h
            ${PROJECT_SOURCE_DIR}/src/session/InteractiveCommandProfile.cpp
            ${PROJECT_SOURCE_DIR}/src/session/InteractiveCommandProfile.h
            ${PROJECT_SOURCE_DIR}/src/session/InteractiveStreamFramer.cpp
            ${PROJECT_SOURCE_DIR}/src/session/InteractiveStreamFramer.h
            ${PROJECT_SOURCE_DIR}/src/session/SessionCommandCoordinator.cpp
            ${PROJECT_SOURCE_DIR}/src/session/SessionCommandCoordinator.h
            ${PROJECT_SOURCE_DIR}/src/session/InteractiveSessionCommandExecutor.cpp
            ${PROJECT_SOURCE_DIR}/src/session/InteractiveSessionCommandExecutor.h
            ${PROJECT_SOURCE_DIR}/src/session/ISessionCommandExecutor.h
            ${PROJECT_SOURCE_DIR}/src/session/CommandPlatformProfile.cpp
            ${PROJECT_SOURCE_DIR}/src/session/CommandPlatformProfile.h
            ${PROJECT_SOURCE_DIR}/src/session/SessionCommandFacade.cpp
            ${PROJECT_SOURCE_DIR}/src/session/SessionCommandFacade.h
            ${PROJECT_SOURCE_DIR}/src/session/TerminalSession.cpp
            ${PROJECT_SOURCE_DIR}/src/session/TerminalSession.h
            ${PROJECT_SOURCE_DIR}/src/service/ConfigManager.cpp
            ${PROJECT_SOURCE_DIR}/src/service/ConfigManager.h
            ${PROJECT_SOURCE_DIR}/src/service/LanguageManager.cpp
            ${PROJECT_SOURCE_DIR}/src/service/LanguageManager.h
            ${PROJECT_SOURCE_DIR}/src/ui/terminal/TerminalView.cpp
            ${PROJECT_SOURCE_DIR}/src/ui/terminal/TerminalView.h
            ${PROJECT_SOURCE_DIR}/src/ui/widgets/SshHostKeyDialog.cpp
            ${PROJECT_SOURCE_DIR}/src/ui/widgets/SshHostKeyDialog.h
        )
        novaterm_add_scheme_resources(novaterm_terminal_session_tests)
        target_include_directories(novaterm_terminal_session_tests PRIVATE
            ${PROJECT_SOURCE_DIR}/src
            "${CMAKE_PREFIX_PATH}/include/QtGui/${Qt6_VERSION}/QtGui"
        )
        target_link_libraries(novaterm_terminal_session_tests
            PRIVATE
                novaterm_core
                ElaWidgetTools
                Qt${QT_VERSION_MAJOR}::Core
                Qt${QT_VERSION_MAJOR}::Gui
                Qt${QT_VERSION_MAJOR}::Widgets
                Qt${QT_VERSION_MAJOR}::SerialPort
                Qt${QT_VERSION_MAJOR}::Test
                ssh::ssh
                OpenSSL::Crypto
                OpenSSL::SSL
        )
        if(WIN32)
            target_link_libraries(novaterm_terminal_session_tests
                PRIVATE ws2_32 crypt32 user32)
        endif()
        qt_add_shaders(novaterm_terminal_session_tests
            novaterm_terminal_session_test_shaders
            BASE ${PROJECT_SOURCE_DIR}
            PREFIX "/shaders"
            FILES
                ${PROJECT_SOURCE_DIR}/src/renderer/shaders/terminal_texture.vert
                ${PROJECT_SOURCE_DIR}/src/renderer/shaders/terminal_texture.frag
        )
        add_custom_command(TARGET novaterm_terminal_session_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_novaterm_clink_src_dir}/clink.bat"
                "${_novaterm_clink_src_dir}/clink.lua"
                "${_novaterm_clink_src_dir}/clink_x64.exe"
                "${_novaterm_clink_src_dir}/clink_dll_x64.dll"
                $<TARGET_FILE_DIR:novaterm_terminal_session_tests>
        )
        add_dependencies(novaterm_terminal_session_tests
            novaterm_conpty_test_child
        )
        add_test(NAME novaterm_terminal_session_tests
                 COMMAND novaterm_terminal_session_tests)
        set_tests_properties(novaterm_terminal_session_tests
                             PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
                                 LABELS "integration;terminal-session"
                                 TIMEOUT 120)
        add_dependencies(check novaterm_terminal_session_tests)
    endif()

    # ── SshTransport 失败路径控制台检查（无需真实 SSH 服务器）──
    add_executable(novaterm_ssh_transport_check
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/SshTransportFailureCheck.cpp
        # SshTransport.cpp 给工作线程设置线程名；本目标直接编译生产源文件、
        # 不链接 novaterm_core，故显式带上该实现。
        ${PROJECT_SOURCE_DIR}/src/core/ThreadNaming.cpp
        ${PROJECT_SOURCE_DIR}/src/transport/SshCommandCompletion.h
        ${PROJECT_SOURCE_DIR}/src/transport/SshMonitorProtocol.cpp
        ${PROJECT_SOURCE_DIR}/src/transport/SshMonitorProtocol.h
        ${PROJECT_SOURCE_DIR}/src/transport/SshTransport.cpp
        ${PROJECT_SOURCE_DIR}/src/transport/SshTransport.h
        ${PROJECT_SOURCE_DIR}/src/transport/ITransport.h
        ${PROJECT_SOURCE_DIR}/src/session/SessionTypes.h
    )
    target_include_directories(novaterm_ssh_transport_check
        PRIVATE ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(novaterm_ssh_transport_check
        PRIVATE
            Qt${QT_VERSION_MAJOR}::Core
            Qt${QT_VERSION_MAJOR}::SerialPort
            ssh::ssh
            OpenSSL::Crypto
            OpenSSL::SSL
    )
    if(WIN32)
        target_link_libraries(novaterm_ssh_transport_check
            PRIVATE ws2_32 crypt32 user32)
    endif()
    add_test(NAME novaterm_ssh_transport_check
             COMMAND novaterm_ssh_transport_check)
    set_tests_properties(novaterm_ssh_transport_check
                         PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
                             LABELS "transport;ssh"
                             TIMEOUT 30)
    add_dependencies(check novaterm_ssh_transport_check)

    # 使用本机历史会话的 SSH 资源监控人工集成检查；不注册到 ctest。
    add_executable(novaterm_ssh_monitor_integration_check
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/SshMonitorIntegrationCheck.cpp
        # 同上：SshTransport.cpp 依赖线程命名工具。
        ${PROJECT_SOURCE_DIR}/src/core/ThreadNaming.cpp
        ${PROJECT_SOURCE_DIR}/src/credential/CredentialStore.cpp
        ${PROJECT_SOURCE_DIR}/src/credential/CredentialStore.h
        ${PROJECT_SOURCE_DIR}/src/profile/ProfileStore.cpp
        ${PROJECT_SOURCE_DIR}/src/profile/ProfileStore.h
        ${PROJECT_SOURCE_DIR}/src/session/SessionStore.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionStore.h
        ${PROJECT_SOURCE_DIR}/src/session/SessionTypes.h
        ${PROJECT_SOURCE_DIR}/src/transport/SshMonitorProtocol.cpp
        ${PROJECT_SOURCE_DIR}/src/transport/SshMonitorProtocol.h
        ${PROJECT_SOURCE_DIR}/src/transport/SshCommandCompletion.h
        ${PROJECT_SOURCE_DIR}/src/transport/SshTransport.cpp
        ${PROJECT_SOURCE_DIR}/src/transport/SshTransport.h
        ${PROJECT_SOURCE_DIR}/src/transport/ITransport.h
    )
    target_include_directories(novaterm_ssh_monitor_integration_check
        PRIVATE ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(novaterm_ssh_monitor_integration_check
        PRIVATE
            Qt${QT_VERSION_MAJOR}::Core
            Qt${QT_VERSION_MAJOR}::SerialPort
            ${NOVATERM_SECRET_SERVICE_LIBS}
            ssh::ssh
            OpenSSL::Crypto
            OpenSSL::SSL
    )
    if(WIN32)
        target_link_libraries(novaterm_ssh_monitor_integration_check
            PRIVATE ws2_32 crypt32 user32 advapi32)
    endif()

    # UI 回归：终端标签动作/紧凑标题、系统信息滚动范围，以及 SSH 主机密钥
    # 对话框的 Ela 控件与端点标题。仅需 Qt Widgets + offscreen。
    add_executable(novaterm_ui_dialog_layout_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/SystemInformationDialogLayoutTests.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/widgets/TerminalSchemeSettings.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/widgets/TerminalSchemeSettings.h
        ${PROJECT_SOURCE_DIR}/src/service/ConfigManager.cpp
        ${PROJECT_SOURCE_DIR}/src/service/ConfigManager.h
        ${PROJECT_SOURCE_DIR}/src/service/TerminalSchemeStore.cpp
        ${PROJECT_SOURCE_DIR}/src/service/TerminalSchemeStore.h
        ${PROJECT_SOURCE_DIR}/src/renderer/TerminalColorScheme.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/TerminalColorScheme.h
        ${PROJECT_SOURCE_DIR}/src/ui/widgets/TerminalTabWidget.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/widgets/TerminalTabWidget.h
        ${PROJECT_SOURCE_DIR}/src/ui/widgets/SshHostKeyDialog.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/widgets/SshHostKeyDialog.h
        ${PROJECT_SOURCE_DIR}/src/ui/widgets/SystemInformationDialog.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/widgets/SystemInformationDialog.h
        ${PROJECT_SOURCE_DIR}/src/service/LanguageManager.cpp
        ${PROJECT_SOURCE_DIR}/src/service/LanguageManager.h
    )
    novaterm_add_scheme_resources(novaterm_ui_dialog_layout_tests)
    target_include_directories(novaterm_ui_dialog_layout_tests
        PRIVATE ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(novaterm_ui_dialog_layout_tests
        PRIVATE
            ElaWidgetTools
            Qt${QT_VERSION_MAJOR}::Core
            Qt${QT_VERSION_MAJOR}::Gui
            Qt${QT_VERSION_MAJOR}::SerialPort
            Qt${QT_VERSION_MAJOR}::Widgets
    )
    add_test(NAME novaterm_ui_dialog_layout_tests
             COMMAND novaterm_ui_dialog_layout_tests)
    set_tests_properties(novaterm_ui_dialog_layout_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        LABELS "ui"
        TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    add_dependencies(check novaterm_ui_dialog_layout_tests)

    # ── TelnetTransport 协议与失败路径测试 ──
    # 用 loopback QTcpServer 充当服务端，不需要真实 telnetd。
    qt_add_executable(novaterm_telnet_transport_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/TelnetTransportTests.cpp
        ${PROJECT_SOURCE_DIR}/src/transport/TelnetTransport.cpp
        ${PROJECT_SOURCE_DIR}/src/transport/TelnetTransport.h
        ${PROJECT_SOURCE_DIR}/src/transport/ITransport.h
        ${PROJECT_SOURCE_DIR}/src/session/SessionTypes.h
    )
    target_include_directories(novaterm_telnet_transport_tests
        PRIVATE ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(novaterm_telnet_transport_tests
        PRIVATE
            Qt${QT_VERSION_MAJOR}::Core
            Qt${QT_VERSION_MAJOR}::Network
            Qt${QT_VERSION_MAJOR}::Test
            # SessionTypes.h 引入 QSerialPort 的枚举（SerialConfig）。
            Qt${QT_VERSION_MAJOR}::SerialPort
            libtelnet
    )
    add_test(NAME novaterm_telnet_transport_tests
             COMMAND novaterm_telnet_transport_tests)
    set_tests_properties(novaterm_telnet_transport_tests
                         PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
                             LABELS "transport;telnet"
                             TIMEOUT 60)
    add_dependencies(check novaterm_telnet_transport_tests)
endif()

if(NOVATERM_BUILD_BENCHMARKS)
    add_executable(novaterm_renderer_p5_gpu_benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/RendererP5GpuBenchmark.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/RenderCommandBuffer.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/RenderScheduler.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/TerminalColorScheme.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/TerminalRenderer.cpp
        ${NOVATERM_P5_RENDERER_SOURCES}
    )
    target_include_directories(novaterm_renderer_p5_gpu_benchmark PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        "${CMAKE_PREFIX_PATH}/include/QtGui/${Qt6_VERSION}/QtGui"
    )
    target_link_libraries(novaterm_renderer_p5_gpu_benchmark PRIVATE
        novaterm_core
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui
        Qt${QT_VERSION_MAJOR}::Widgets
    )
    qt_add_shaders(novaterm_renderer_p5_gpu_benchmark
        novaterm_renderer_p5_gpu_benchmark_shaders
        BASE ${PROJECT_SOURCE_DIR}
        PREFIX "/shaders"
        FILES
            ${PROJECT_SOURCE_DIR}/src/renderer/shaders/terminal_texture.vert
            ${PROJECT_SOURCE_DIR}/src/renderer/shaders/terminal_texture.frag
    )
    if(BUILD_TESTING AND NOVATERM_RUN_GPU_ACCEPTANCE_TESTS)
        add_test(NAME novaterm_renderer_p5_gpu_acceptance
            COMMAND novaterm_renderer_p5_gpu_benchmark
                --duration-ms 5000
                --refresh-rate 60
                --scrollback-limit 100000
                --prefill-lines 100000
        )
        set_tests_properties(novaterm_renderer_p5_gpu_acceptance
            PROPERTIES
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
                LABELS "integration;renderer;gpu;benchmark"
                RUN_SERIAL TRUE
                TIMEOUT 180
        )
        add_dependencies(check novaterm_renderer_p5_gpu_benchmark)
    endif()

    add_executable(novaterm_renderer_p3_gpu_benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/RendererP3GpuBenchmark.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/RenderCommandBuffer.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/RenderCommandBuffer.h
        ${PROJECT_SOURCE_DIR}/src/renderer/RenderScheduler.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/RenderScheduler.h
        ${PROJECT_SOURCE_DIR}/src/renderer/TerminalColorScheme.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/TerminalColorScheme.h
        ${PROJECT_SOURCE_DIR}/src/renderer/TerminalRenderer.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/TerminalRenderer.h
        ${NOVATERM_P5_RENDERER_SOURCES}
    )
    target_include_directories(novaterm_renderer_p3_gpu_benchmark PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        "${CMAKE_PREFIX_PATH}/include/QtGui/${Qt6_VERSION}/QtGui"
    )
    target_link_libraries(novaterm_renderer_p3_gpu_benchmark
        PRIVATE
            novaterm_core
            Qt${QT_VERSION_MAJOR}::Core
            Qt${QT_VERSION_MAJOR}::Gui
            Qt${QT_VERSION_MAJOR}::Widgets
    )
    qt_add_shaders(novaterm_renderer_p3_gpu_benchmark
        novaterm_renderer_p3_gpu_benchmark_shaders
        BASE ${PROJECT_SOURCE_DIR}
        PREFIX "/shaders"
        FILES
            ${PROJECT_SOURCE_DIR}/src/renderer/shaders/terminal_texture.vert
            ${PROJECT_SOURCE_DIR}/src/renderer/shaders/terminal_texture.frag
    )

    add_executable(novaterm_residue_repro
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/ResidueRepro.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/RenderCommandBuffer.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/RenderCommandBuffer.h
        ${PROJECT_SOURCE_DIR}/src/renderer/RenderScheduler.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/RenderScheduler.h
        ${PROJECT_SOURCE_DIR}/src/renderer/TerminalColorScheme.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/TerminalColorScheme.h
        ${PROJECT_SOURCE_DIR}/src/renderer/TerminalRenderer.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/TerminalRenderer.h
        ${NOVATERM_P5_RENDERER_SOURCES}
    )
    target_include_directories(novaterm_residue_repro PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        "${CMAKE_PREFIX_PATH}/include/QtGui/${Qt6_VERSION}/QtGui"
    )
    target_link_libraries(novaterm_residue_repro
        PRIVATE
            novaterm_core
            Qt${QT_VERSION_MAJOR}::Core
            Qt${QT_VERSION_MAJOR}::Gui
            Qt${QT_VERSION_MAJOR}::Widgets
    )
    qt_add_shaders(novaterm_residue_repro
        novaterm_residue_repro_shaders
        BASE ${PROJECT_SOURCE_DIR}
        PREFIX "/shaders"
        FILES
            ${PROJECT_SOURCE_DIR}/src/renderer/shaders/terminal_texture.vert
            ${PROJECT_SOURCE_DIR}/src/renderer/shaders/terminal_texture.frag
    )

    add_executable(novaterm_renderer_p3_benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/RendererP3Benchmark.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/RenderCommandBuffer.cpp
        ${PROJECT_SOURCE_DIR}/src/renderer/RenderCommandBuffer.h
    )
    target_include_directories(novaterm_renderer_p3_benchmark
        PRIVATE ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(novaterm_renderer_p3_benchmark
        PRIVATE
            novaterm_core
            Qt${QT_VERSION_MAJOR}::Core
            Qt${QT_VERSION_MAJOR}::Gui
    )

    add_executable(novaterm_core_benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/CoreBenchmark.cpp
    )
    target_link_libraries(novaterm_core_benchmark PRIVATE novaterm_core)

    add_executable(novaterm_scrollback_benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/ScrollbackBenchmark.cpp
    )
    target_link_libraries(novaterm_scrollback_benchmark PRIVATE novaterm_core)

    # tailFrom near-tail 缩放护栏：与其它 benchmark 不同，这是带 PASS/FAIL 的
    # 回归门（尾部访问必须与分块数解耦，保护 f72bad4 的双端 lineAt）。用比值
    # 判据、与绝对机器速度无关，故注册为 CTest 测试；回退到 O(chunks) 时退出码
    # 非零即失败。运行数秒。
    add_test(NAME novaterm_scrollback_tailfrom_scale
        COMMAND novaterm_scrollback_benchmark --tailfrom-scale)
    set_tests_properties(novaterm_scrollback_tailfrom_scale PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        LABELS "scrollback;perf")

    add_custom_target(scrollback_benchmark_matrix
        COMMAND $<TARGET_FILE:novaterm_scrollback_benchmark>
            --matrix
            --matrix-lines 1000,10000,100000,1000000
            --columns 80
            --chunk-lines 1024
            --max-bytes 268435456
        DEPENDS novaterm_scrollback_benchmark
        USES_TERMINAL
        COMMENT "Running isolated NovaTerm P4 scrollback size/RSS/latency matrix"
    )

    # Benchmark 使用自己的分区性能报告，不经过 CTest。参数对应 P2/P4
    # 设计文档中的 20 MiB Parser 与 10 万行 Scrollback 基线。
    add_custom_target(benchmarks
        COMMAND $<TARGET_FILE:novaterm_renderer_p3_benchmark>
            --iterations 20000
        COMMAND $<TARGET_FILE:novaterm_core_benchmark>
            --bytes 20971520
            --lines 100000
        COMMAND $<TARGET_FILE:novaterm_scrollback_benchmark>
            --lines 100000
            --columns 80
            --chunk-lines 1024
            --max-bytes 268435456
        DEPENDS
            novaterm_core_benchmark
            novaterm_renderer_p3_benchmark
            novaterm_scrollback_benchmark
        USES_TERMINAL
        COMMENT "Running NovaTerm P2/P4 performance benchmarks"
    )
endif()
