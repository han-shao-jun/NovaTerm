# lxqt2-build-tools version file — satisfies QTermWidget's version requirement
set(PACKAGE_VERSION "2.4.0")

if(NOT "${PACKAGE_FIND_VERSION}" STREQUAL "")
    if(PACKAGE_FIND_VERSION VERSION_GREATER PACKAGE_VERSION)
        set(PACKAGE_VERSION_COMPATIBLE FALSE)
    else()
        set(PACKAGE_VERSION_COMPATIBLE TRUE)
        if(PACKAGE_FIND_VERSION STREQUAL PACKAGE_VERSION)
            set(PACKAGE_VERSION_EXACT TRUE)
        endif()
    endif()
endif()
