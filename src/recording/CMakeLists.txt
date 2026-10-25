# Persistent receive-stream recording; independent of MCP and the LZ4 scrollback backend.
qt_add_library(novaterm_history_recording STATIC
    HistoryTypes.h
    HistoryRecorder.cpp HistoryRecorder.h
    HistoryChunkStore.cpp HistoryChunkStore.h
    HistoryIndex.cpp HistoryIndex.h detail/HistoryIndexSql.h
    HistoryStorageIo.cpp HistoryStorageIo.h
    HistoryRetentionPolicy.cpp HistoryRetentionPolicy.h
    HistoryService.cpp HistoryService.h)
target_include_directories(novaterm_history_recording PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(novaterm_history_recording PUBLIC Qt6::Core PRIVATE Qt6::Sql Threads::Threads)
if(WIN32)
    target_link_libraries(novaterm_history_recording PRIVATE advapi32)
endif()
