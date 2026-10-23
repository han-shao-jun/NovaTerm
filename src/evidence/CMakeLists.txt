qt_add_library(novaterm_evidence STATIC EvidenceStore.cpp ImportedResourceStore.cpp)
target_include_directories(novaterm_evidence PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(novaterm_evidence PUBLIC Qt6::Core)
