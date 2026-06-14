# FindQTermWidget.cmake
# Locate QTermWidget library

find_path(QTERMWIDGET_INCLUDE_DIR qtermwidget5/qtermwidget.h
    HINTS /usr/include /usr/local/include
    PATH_SUFFIXES qtermwidget5)

find_library(QTERMWIDGET_LIBRARY
    NAMES qtermwidget5 qtermwidget6
    HINTS /usr/lib /usr/local/lib)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(QTermWidget
    REQUIRED_VARS QTERMWIDGET_LIBRARY QTERMWIDGET_INCLUDE_DIR)

if(QTermWidget_FOUND AND NOT TARGET QTermWidget::QTermWidget)
    add_library(QTermWidget::QTermWidget UNKNOWN IMPORTED)
    set_target_properties(QTermWidget::QTermWidget PROPERTIES
        IMPORTED_LOCATION "${QTERMWIDGET_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${QTERMWIDGET_INCLUDE_DIR}")
endif()
