# 协议单测使用标准库，不链接 Qt；根工程与独立构建共享目标声明。
function(novaterm_filetransfer_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE novaterm_filetransfer)
    set_target_properties(${target} PROPERTIES
        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF CXX_EXTENSIONS OFF
        WIN32_EXECUTABLE FALSE
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES LABELS "unit;filetransfer;p9" TIMEOUT 60)
    if(TARGET check)
        add_dependencies(check ${target})
    endif()
endfunction()
novaterm_filetransfer_test(novaterm_filetransfer_checksum_tests ChecksumTests.cpp)
novaterm_filetransfer_test(novaterm_filetransfer_support_tests EngineSupportTests.cpp)
novaterm_filetransfer_test(novaterm_xmodem_tests XmodemTests.cpp)
novaterm_filetransfer_test(novaterm_ymodem_tests YmodemTests.cpp)
novaterm_filetransfer_test(novaterm_zmodem_tests ZmodemTests.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(novaterm_filetransfer_interop_driver InteropDriver.cpp)
    target_link_libraries(novaterm_filetransfer_interop_driver PRIVATE novaterm_filetransfer)
    set_target_properties(novaterm_filetransfer_interop_driver PROPERTIES
        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF CXX_EXTENSIONS OFF
    )
    target_compile_options(novaterm_filetransfer_interop_driver PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
novaterm_filetransfer_test(novaterm_filetransfer_no_qt_link_check NoQtLinkCheck.cpp)
