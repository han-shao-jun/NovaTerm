add_library(novaterm_resource_sharing STATIC
    ResourceSharingService.h ResourceSharingService.cpp HistoryCapabilities.h HistoryCapabilities.cpp
    RecordingHistoryReadAdapter.h)
target_include_directories(novaterm_resource_sharing PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(novaterm_resource_sharing PUBLIC novaterm_history_search novaterm_evidence novaterm_capabilities Qt6::Core)
set_target_properties(novaterm_resource_sharing PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_compile_options(novaterm_resource_sharing PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
