cmake_minimum_required(VERSION 3.20)
project(NovaTermSerialStress LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets SerialPort ShaderTools)
qt_standard_project_setup(REQUIRES 6.8)
if(NOT WIN32)
    message(FATAL_ERROR "此验收夹具使用 Win32 CPU/RSS 统计，只支持 Windows")
endif()
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(PRODUCTION_BUILD "${REPO}/build/Release" CACHE PATH "已构建的 Release 工程")
option(NOVATERM_STRESS_PROFILE_SYMBOLS "独立重建带符号的优化 Core 和 libvterm，用于热点采样" OFF)
if(NOVATERM_STRESS_PROFILE_SYMBOLS)
    add_subdirectory("${REPO}/third_party/libvterm-0.3.3" vterm-profile)
    set_target_properties(libvterm PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    set(_stress_vterm libvterm)
    qt_add_library(stress_core STATIC
        ${REPO}/src/core/ThreadNaming.cpp
        ${REPO}/src/core/terminal/BoundedByteQueue.cpp
        ${REPO}/src/core/terminal/KeyMapper.cpp
        ${REPO}/src/core/terminal/ScreenBuffer.cpp
        ${REPO}/src/core/terminal/ScrollbackBuffer.cpp
        ${REPO}/src/core/terminal/TerminalCore.cpp
        ${REPO}/src/core/terminal/VTAdapter.cpp
        ${REPO}/src/core/scrollback/ChunkedScrollback.cpp
        ${REPO}/src/core/scrollback/LineLayout.cpp
        ${REPO}/src/core/scrollback/ScrollbackChunk.cpp
        ${REPO}/src/core/scrollback/ScrollbackSnapshot.cpp
        ${REPO}/src/core/search/SearchEngine.cpp)
    target_include_directories(stress_core PUBLIC "${REPO}/src")
    target_link_libraries(stress_core PUBLIC Qt6::Core Qt6::Gui PRIVATE libvterm)
    target_compile_definitions(stress_core PRIVATE NOMINMAX QT_NO_DEBUG_OUTPUT)
    if(MSVC)
        target_compile_options(stress_core PRIVATE /Z7 /utf-8)
        target_compile_options(libvterm PRIVATE /Z7 /utf-8)
    endif()
else()
    add_library(stress_core STATIC IMPORTED)
    set_target_properties(stress_core PROPERTIES IMPORTED_LOCATION "${PRODUCTION_BUILD}/bin/novaterm_core.lib")
    set(_stress_vterm "${PRODUCTION_BUILD}/bin/libvterm.lib")
endif()
qt_add_executable(serial_stress
    main.cpp
    ${REPO}/src/transport/SerialTransport.cpp
    ${REPO}/src/transport/ITransport.h
    ${REPO}/src/session/SessionInputPump.cpp
    ${REPO}/src/session/InteractiveStreamFramer.cpp
    ${REPO}/src/session/InteractiveCommandProfile.cpp
    ${REPO}/src/renderer/RenderCommandBuffer.cpp
    ${REPO}/src/renderer/RenderScheduler.cpp
    ${REPO}/src/renderer/TerminalColorScheme.cpp
    ${REPO}/src/renderer/TerminalRenderer.cpp
    ${REPO}/src/renderer/font/FontManager.cpp
    ${REPO}/src/renderer/glyph/GlyphAtlas.cpp
    ${REPO}/src/renderer/glyph/GlyphCache.cpp
    ${REPO}/src/renderer/gpu/BufferBudget.cpp
    ${REPO}/src/renderer/gpu/MaterialBatcher.cpp
    ${REPO}/src/renderer/gpu/RendererCapabilities.cpp
    ${REPO}/src/renderer/gpu/RowSlotMap.cpp
    ${REPO}/src/renderer/glyph/GlyphRasterizer.cpp)
set_target_properties(serial_stress PROPERTIES WIN32_EXECUTABLE FALSE)
target_compile_definitions(serial_stress PRIVATE NOVATERM_SERIAL_STRESS)
target_include_directories(serial_stress PRIVATE "${REPO}/src"
    "${Qt6Gui_INCLUDE_DIRS}")
target_link_libraries(serial_stress PRIVATE stress_core
    ${_stress_vterm}
    Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Widgets Qt6::SerialPort winmm psapi)
if(MSVC)
    target_compile_options(serial_stress PRIVATE /W4 /utf-8)
    if(NOVATERM_STRESS_PROFILE_SYMBOLS)
        target_compile_options(serial_stress PRIVATE /Z7)
        target_link_options(serial_stress PRIVATE /DEBUG:FULL /INCREMENTAL:NO /OPT:REF /OPT:ICF)
    endif()
endif()
qt_add_shaders(serial_stress stress_shaders BASE "${REPO}" PREFIX "/shaders"
    FILES "${REPO}/src/renderer/shaders/terminal_texture.vert"
          "${REPO}/src/renderer/shaders/terminal_texture.frag")
