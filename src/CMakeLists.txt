file(GLOB_RECURSE SOURCES *.cpp *.h)

add_executable(NovaTerm ${SOURCES} ${CMAKE_SOURCE_DIR}/resources/resources.qrc)

target_include_directories(NovaTerm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(NovaTerm PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Sql
    Qt${QT_VERSION_MAJOR}::Svg
    ElaWidgetTools
    ${QTERMWIDGET_TARGET}
)

if(WIN32)
    target_compile_definitions(NovaTerm PRIVATE _WIN32 _WIN64)
    set_target_properties(NovaTerm PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
