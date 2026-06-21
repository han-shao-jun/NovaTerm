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
    if(CMAKE_BUILD_TYPE MATCHES "Release")
        #发布版除去黑窗口，无qDebu打印
        add_compile_definitions(QT_NO_DEBUG_OUTPUT)
        set_target_properties(NovaTerm PROPERTIES WIN32_EXECUTABLE TRUE)
    else ()
        #调试版保留黑窗口,有qDebug打印
    endif()
    target_compile_definitions(NovaTerm PRIVATE _WIN32 _WIN64)
endif()
