# All dependencies in this file are local repository sources. Do not add FetchContent.

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

add_library(kvmux_nlohmann_json INTERFACE)
target_include_directories(kvmux_nlohmann_json SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/nlohmann_json/include")
add_library(nlohmann_json::nlohmann_json ALIAS kvmux_nlohmann_json)

add_library(kvmux_spdlog INTERFACE)
target_compile_definitions(kvmux_spdlog INTERFACE SPDLOG_HEADER_ONLY)
target_include_directories(kvmux_spdlog SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/spdlog/include")
add_library(spdlog::spdlog_header_only ALIAS kvmux_spdlog)
