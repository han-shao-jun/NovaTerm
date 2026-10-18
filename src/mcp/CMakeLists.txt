# 纯值类型：ID、绑定、请求、错误与预算；不依赖协议、HTTP、Session 或 Widgets。
qt_add_library(novaterm_mcp_contracts STATIC
    contracts/Identifiers.cpp contracts/Identifiers.h
    contracts/Errors.cpp contracts/Errors.h
    contracts/Budget.h
    contracts/TaskPhases.h
    contracts/SessionBinding.h
    contracts/Permissions.h
    contracts/ToolRequests.h)
target_include_directories(novaterm_mcp_contracts PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(novaterm_mcp_contracts PUBLIC Qt6::Core)

# 纯应用服务：授权事务与运行期授权存储；不依赖 Session、HTTP 或 Widgets。
qt_add_library(novaterm_mcp_application STATIC
    application/GrantStore.cpp application/GrantStore.h
    application/AuthorizationService.cpp application/AuthorizationService.h
    application/CapabilityResolver.cpp application/CapabilityResolver.h
    application/RequestScheduler.cpp application/RequestScheduler.h
    application/CommandDedup.cpp application/CommandDedup.h
    application/CaptureSearch.cpp application/CaptureSearch.h
    application/CaptureStore.cpp application/CaptureStore.h
    application/SnapshotStore.h
    application/ObservationState.cpp application/ObservationState.h
    tasks/CommandRuntime.cpp tasks/CommandRuntime.h
    tasks/TargetReservation.cpp tasks/TargetReservation.h
    CommandRiskPolicy.cpp CommandRiskPolicy.h
    tasks/ReadUseCases.cpp tasks/ReadUseCases.h
    tasks/ConfirmationRegistry.cpp tasks/ConfirmationRegistry.h
    security/ClientRegistry.cpp security/ClientRegistry.h)
target_include_directories(novaterm_mcp_application PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(novaterm_mcp_application PUBLIC novaterm_mcp_contracts Qt6::Core)

# MCP wire 协议库不依赖终端或 UI，由进程内 HTTP 服务复用。
qt_add_library(novaterm_mcp_protocol STATIC McpProtocol.cpp McpProtocol.h
    protocol/ToolCodec.cpp protocol/ToolCodec.h
    protocol/BackendEvents.h
    protocol/ProtocolProjectionSizer.cpp protocol/ProtocolProjectionSizer.h
    WireSession.cpp WireSession.h ElicitationBroker.cpp ElicitationBroker.h)
target_include_directories(novaterm_mcp_protocol PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(novaterm_mcp_protocol PUBLIC novaterm_mcp_contracts Qt6::Core Qt6::Network)
target_compile_definitions(novaterm_mcp_protocol PRIVATE NOVATERM_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_link_libraries(novaterm_mcp_protocol PRIVATE advapi32)
endif()

qt_add_library(novaterm_mcp_http STATIC
    HttpMcpServer.cpp HttpMcpServer.h
    HttpMcpEndpoint.cpp HttpMcpEndpoint.h)
target_link_libraries(novaterm_mcp_http PUBLIC novaterm_mcp_protocol Qt6::HttpServer)
target_link_libraries(novaterm_mcp_http PRIVATE novaterm_core)

# Session 自动化组件独立于 MCP 协议/HTTP/UI；runtime 只做组合与适配。
# 源文件仍在 src/session/（既有 Framer/Arbiter/Provider 不机械搬迁）。
qt_add_library(novaterm_session_automation STATIC
    ${PROJECT_SOURCE_DIR}/src/session/SessionDirectory.cpp
    ${PROJECT_SOURCE_DIR}/src/session/SessionDirectory.h
    ${PROJECT_SOURCE_DIR}/src/session/ISessionCommandExecutor.h
    ${PROJECT_SOURCE_DIR}/src/session/CommandPlatformProfile.cpp
    ${PROJECT_SOURCE_DIR}/src/session/CommandPlatformProfile.h
    ${PROJECT_SOURCE_DIR}/src/session/LocalSessionCommandExecutor.cpp
    ${PROJECT_SOURCE_DIR}/src/session/LocalSessionCommandExecutor.h
    ${PROJECT_SOURCE_DIR}/src/session/SessionCommandFacade.cpp
    ${PROJECT_SOURCE_DIR}/src/session/SessionCommandFacade.h
    ${PROJECT_SOURCE_DIR}/src/session/SshSessionCommandExecutor.cpp
    ${PROJECT_SOURCE_DIR}/src/session/SshSessionCommandExecutor.h
)
target_include_directories(novaterm_session_automation PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(novaterm_session_automation PUBLIC
    novaterm_core Qt6::Core Qt6::SerialPort)

qt_add_library(novaterm_mcp_runtime STATIC
    McpAccess.cpp McpAccess.h
    CommandPolicy.cpp CommandPolicy.h
    McpService.cpp McpService.h
    CommandSessionAdapter.cpp CommandSessionAdapter.h
    CapabilityAdapter.cpp CapabilityAdapter.h
    ObservationService.cpp ObservationService.h
    TerminalInteraction.cpp TerminalInteraction.h
)
target_link_libraries(novaterm_mcp_runtime PUBLIC
    novaterm_mcp_application novaterm_session_automation
    novaterm_core Qt6::SerialPort
    PRIVATE novaterm_mcp_http novaterm_mcp_protocol)
target_compile_definitions(novaterm_mcp_runtime PRIVATE NOVATERM_VERSION="${PROJECT_VERSION}")
