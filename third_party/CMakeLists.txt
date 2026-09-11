# All dependencies in this file are local repository sources. Do not add FetchContent.

if(KVMUX_BUILD_GUI)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(SDL EXCLUDE_FROM_ALL)

    add_library(kvmux_glad STATIC glad/src/gl.c)
    target_include_directories(kvmux_glad PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/glad/include")
    target_link_libraries(kvmux_glad PUBLIC ${CMAKE_DL_LIBS})
    add_library(kvmux::glad ALIAS kvmux_glad)

    add_library(kvmux_imgui STATIC
        imgui/imgui.cpp
        imgui/imgui_draw.cpp
        imgui/imgui_tables.cpp
        imgui/imgui_widgets.cpp
        imgui/backends/imgui_impl_sdl3.cpp
        imgui/backends/imgui_impl_opengl3.cpp)
    target_include_directories(kvmux_imgui SYSTEM PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/imgui"
        "${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends")
    target_link_libraries(kvmux_imgui PUBLIC SDL3::SDL3)
    add_library(kvmux::imgui ALIAS kvmux_imgui)
endif()

add_library(kvmux_nlohmann_json INTERFACE)
target_include_directories(kvmux_nlohmann_json SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/nlohmann_json/include")
add_library(nlohmann_json::nlohmann_json ALIAS kvmux_nlohmann_json)

add_library(kvmux_spdlog INTERFACE)
target_compile_definitions(kvmux_spdlog INTERFACE SPDLOG_HEADER_ONLY)
target_include_directories(kvmux_spdlog SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/spdlog/include")
add_library(spdlog::spdlog_header_only ALIAS kvmux_spdlog)

# libserialport 0.1.2 ships autotools and Visual Studio project files. This
# target is the KVMux CMake integration for the unchanged upstream sources.
add_library(kvmux_libserialport STATIC
    libserialport/serialport.c
    libserialport/timing.c)
target_include_directories(kvmux_libserialport SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/libserialport")
if(WIN32)
    target_sources(kvmux_libserialport PRIVATE libserialport/windows.c)
    target_compile_definitions(kvmux_libserialport PRIVATE LIBSERIALPORT_MSBUILD)
    target_link_libraries(kvmux_libserialport PRIVATE setupapi)
elseif(APPLE)
    target_sources(kvmux_libserialport PRIVATE libserialport/macosx.c)
    target_compile_definitions(kvmux_libserialport PRIVATE
        SP_PRIV= HAVE_REALPATH=1 HAVE_SYS_FILE_H=1 HAVE_FLOCK=1)
    target_link_libraries(kvmux_libserialport PRIVATE
        "-framework IOKit" "-framework CoreFoundation")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(kvmux_libserialport PRIVATE
        libserialport/linux.c libserialport/linux_termios.c)
    target_compile_definitions(kvmux_libserialport PRIVATE
        SP_PRIV= HAVE_REALPATH=1 HAVE_SYS_FILE_H=1 HAVE_FLOCK=1
        HAVE_CLOCK_GETTIME=1 HAVE_STRUCT_TERMIOS_C_ISPEED=1
        HAVE_STRUCT_TERMIOS_C_OSPEED=1 HAVE_TERMIOS_SPEED=1
        HAVE_STRUCT_TERMIOS2=1 HAVE_STRUCT_SERIAL_STRUCT=1
        HAVE_DECL_BOTHER=1)
else()
    message(FATAL_ERROR "libserialport is supported only on Windows, Linux and macOS")
endif()
add_library(kvmux::libserialport ALIAS kvmux_libserialport)
