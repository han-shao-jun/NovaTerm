add_library(novaterm_evidence STATIC EvidenceTypes.h EvidenceAuthorization.h EvidenceSourceReader.h
    EvidenceStore.h EvidenceStore.cpp ImportedResourceStore.h ImportedResourceStore.cpp)
target_compile_features(novaterm_evidence PUBLIC cxx_std_17)
target_link_libraries(novaterm_evidence PUBLIC Qt6::Core)
target_include_directories(novaterm_evidence PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(novaterm_evidence PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
set_target_properties(novaterm_evidence PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
