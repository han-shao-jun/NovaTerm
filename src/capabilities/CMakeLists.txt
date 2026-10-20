# Pure application admission/catalog. Concrete Session and wire codecs are consumers.
qt_add_library(novaterm_capabilities STATIC
    CapabilityTypes.h CapabilityRegistry.h CapabilityRegistry.cpp
    CapabilityResult.h CapabilityAuthorization.h CapabilityAuthorization.cpp
    CapabilityDispatcher.h CapabilityDispatcher.cpp TypedCapabilityPort.h
    CapabilityEntry.h McpCapabilityAdapter.h LocalAgentAdapter.h)
target_include_directories(novaterm_capabilities PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(novaterm_capabilities PUBLIC Qt6::Core)

# Declared here; dependencies are resolved after src/mcp has declared its application target.
qt_add_library(novaterm_terminal_capabilities STATIC terminal/ReadCapabilities.cpp terminal/ReadCapabilities.h)
target_include_directories(novaterm_terminal_capabilities PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(novaterm_terminal_capabilities PUBLIC novaterm_capabilities novaterm_mcp_application Qt6::Core)

target_sources(novaterm_capabilities PRIVATE
    TargetEnvironment.h SessionExecutionRouter.h SessionExecutionRouter.cpp)
qt_add_library(novaterm_session_environment STATIC
    ${PROJECT_SOURCE_DIR}/src/session/EnvironmentTracker.cpp
    ${PROJECT_SOURCE_DIR}/src/session/EnvironmentTracker.h)
target_link_libraries(novaterm_session_environment PUBLIC novaterm_capabilities Qt6::Core)
target_sources(novaterm_capabilities PRIVATE DiagnosticTaskManager.h DiagnosticTaskManager.cpp)
target_sources(novaterm_terminal_capabilities PRIVATE terminal/CommandCapabilities.h terminal/CommandCapabilities.cpp)
target_sources(novaterm_terminal_capabilities PRIVATE terminal/InputCapabilities.h terminal/InputCapabilities.cpp)

target_sources(novaterm_terminal_capabilities PRIVATE terminal/ObservationCapabilities.h terminal/ObservationCapabilities.cpp)
target_sources(novaterm_terminal_capabilities PRIVATE terminal/SearchCapabilities.h terminal/SearchCapabilities.cpp)
