# Offline history authorization/read/search. Storage is supplied through HistoryReadPort.
# Only recording value types are shared; no concrete recording backend is linked here.
qt_add_library(novaterm_history_search STATIC
    HistoryAuthorizationStore.cpp HistoryAuthorizationStore.h
    HistoryReadService.cpp HistoryReadService.h
    HistorySearchService.cpp HistorySearchService.h
    HistoryProjectionDecoder.cpp HistoryProjectionDecoder.h
    HistorySearchTypes.h)
target_include_directories(novaterm_history_search PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(novaterm_history_search PUBLIC Qt6::Core)
