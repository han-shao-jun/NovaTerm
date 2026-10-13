# MCP wire 协议库不依赖终端或 UI，由进程内 HTTP 服务复用。
qt_add_library(novaterm_mcp_protocol STATIC McpProtocol.cpp McpProtocol.h
    WireSession.cpp WireSession.h ElicitationBroker.cpp ElicitationBroker.h)
target_include_directories(novaterm_mcp_protocol PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(novaterm_mcp_protocol PUBLIC Qt6::Core Qt6::Network)
target_compile_definitions(novaterm_mcp_protocol PRIVATE NOVATERM_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_link_libraries(novaterm_mcp_protocol PRIVATE advapi32)
endif()

qt_add_library(novaterm_mcp_http STATIC
    HttpMcpServer.cpp HttpMcpServer.h
    HttpMcpEndpoint.cpp HttpMcpEndpoint.h)
target_link_libraries(novaterm_mcp_http PUBLIC novaterm_mcp_protocol Qt6::HttpServer)
target_link_libraries(novaterm_mcp_http PRIVATE novaterm_core)

qt_add_library(novaterm_mcp_runtime STATIC
    McpAccess.cpp McpAccess.h
    CommandPolicy.cpp CommandPolicy.h
    CommandRiskPolicy.cpp CommandRiskPolicy.h
    McpService.cpp McpService.h
    ../session/SessionDirectory.cpp ../session/SessionDirectory.h
    ../session/ISessionCommandExecutor.h
    ../session/CommandPlatformProfile.cpp ../session/CommandPlatformProfile.h
    ../session/LocalSessionCommandExecutor.cpp ../session/LocalSessionCommandExecutor.h
    ../session/SessionCommandFacade.cpp ../session/SessionCommandFacade.h
    ../session/SshSessionCommandExecutor.cpp ../session/SshSessionCommandExecutor.h
)
target_link_libraries(novaterm_mcp_runtime PUBLIC
    novaterm_mcp_http novaterm_core Qt6::SerialPort)
target_compile_definitions(novaterm_mcp_runtime PRIVATE NOVATERM_VERSION="${PROJECT_VERSION}")
