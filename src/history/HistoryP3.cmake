qt_add_library(novaterm_history_search STATIC
    ${PROJECT_SOURCE_DIR}/src/history/HistoryAuthorizationStore.cpp
    ${PROJECT_SOURCE_DIR}/src/history/HistoryReadService.cpp
    ${PROJECT_SOURCE_DIR}/src/history/HistorySearchService.cpp
    ${PROJECT_SOURCE_DIR}/src/history/HistoryProjectionDecoder.cpp)
target_include_directories(novaterm_history_search PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(novaterm_history_search PUBLIC Qt6::Core)
