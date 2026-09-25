# MCP 协议库不依赖终端或 UI，供 console 桥接进程单独复用。
qt_add_library(novaterm_mcp_protocol STATIC McpProtocol.cpp McpProtocol.h)
target_include_directories(novaterm_mcp_protocol PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(novaterm_mcp_protocol PUBLIC Qt6::Core Qt6::Network)
if(WIN32)
    target_link_libraries(novaterm_mcp_protocol PRIVATE advapi32)
endif()

qt_add_library(novaterm_mcp_runtime STATIC
    McpAccess.cpp McpAccess.h
    CommandPolicy.cpp CommandPolicy.h
    CommandRiskPolicy.cpp CommandRiskPolicy.h
    LocalMcpServer.cpp LocalMcpServer.h
    McpService.cpp McpService.h
    ../session/SessionDirectory.cpp ../session/SessionDirectory.h
    ../session/ISessionCommandExecutor.h
    ../session/CommandPlatformProfile.cpp ../session/CommandPlatformProfile.h
    ../session/LocalSessionCommandExecutor.cpp ../session/LocalSessionCommandExecutor.h
    ../session/SessionCommandFacade.cpp ../session/SessionCommandFacade.h
    ../session/SshSessionCommandExecutor.cpp ../session/SshSessionCommandExecutor.h
)
target_link_libraries(novaterm_mcp_runtime PUBLIC
    novaterm_mcp_protocol novaterm_core Qt6::SerialPort)
target_compile_definitions(novaterm_mcp_runtime PRIVATE NOVATERM_VERSION="${PROJECT_VERSION}")
