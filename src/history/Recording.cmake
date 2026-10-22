# Intentionally not included by the standalone no-Qt scrollback build.
qt_add_library(novaterm_history_recording STATIC
    ${PROJECT_SOURCE_DIR}/src/history/HistoryTypes.h
    ${PROJECT_SOURCE_DIR}/src/history/HistoryRecorder.cpp
    ${PROJECT_SOURCE_DIR}/src/history/HistoryChunkStore.cpp
    ${PROJECT_SOURCE_DIR}/src/history/HistoryIndex.cpp
    ${PROJECT_SOURCE_DIR}/src/history/HistoryStorageIo.cpp
    ${PROJECT_SOURCE_DIR}/src/history/HistoryRetentionPolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/history/HistoryService.cpp
    ${PROJECT_SOURCE_DIR}/src/history/HistoryService.h)
target_include_directories(novaterm_history_recording PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(novaterm_history_recording PUBLIC Qt6::Core PRIVATE Qt6::Sql Threads::Threads)
if(WIN32)
    target_link_libraries(novaterm_history_recording PRIVATE advapi32)
endif()
