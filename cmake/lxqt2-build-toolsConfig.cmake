# Minimal lxqt2-build-toolsConfig.cmake
# Provides CMake modules for QTermWidget Qt6 build
# Points directly to lxqt-build-tools submodule sources — no build needed

set(PACKAGE_VERSION "2.4.0")
set(PACKAGE_VERSION_COMPATIBLE TRUE)

set(LXQT_CMAKE_MODULES_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/lxqt-build-tools/cmake/modules")
set(LXQT_CMAKE_FIND_MODULES_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/lxqt-build-tools/cmake/find-modules")

list(APPEND CMAKE_MODULE_PATH
    "${LXQT_CMAKE_MODULES_DIR}"
    "${LXQT_CMAKE_FIND_MODULES_DIR}"
)

# Provide the modules that QTermWidget needs
include("${LXQT_CMAKE_MODULES_DIR}/LXQtPreventInSourceBuilds.cmake" OPTIONAL)
include("${LXQT_CMAKE_MODULES_DIR}/LXQtTranslateTs.cmake" OPTIONAL)
include("${LXQT_CMAKE_MODULES_DIR}/LXQtCompilerSettings.cmake" OPTIONAL)
include("${LXQT_CMAKE_MODULES_DIR}/LXQtCreatePkgConfigFile.cmake" OPTIONAL)
