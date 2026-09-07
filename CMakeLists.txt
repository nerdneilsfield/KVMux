cmake_minimum_required(VERSION 3.25)

project(KVMux VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(FFmpeg REQUIRED)

option(KVMUX_ENABLE_SANITIZERS "Enable the supported address and undefined behavior sanitizers" OFF)

add_library(kvmux_core STATIC
    src/control/ch9329_protocol.cpp
    src/control/control_queue.cpp
    src/control/hid_keyboard.cpp
    src/support/build_info.cpp
    src/video/capture_sample.cpp
    src/video/video_processor.cpp
)
add_library(kvmux::core ALIAS kvmux_core)
target_compile_features(kvmux_core PUBLIC cxx_std_20)
target_include_directories(kvmux_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(kvmux_core PUBLIC FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale)

if(MSVC)
    target_compile_options(kvmux_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(kvmux_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

if(KVMUX_ENABLE_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "KVMUX_ENABLE_SANITIZERS is not supported by the MSVC preset")
    endif()
    target_compile_options(kvmux_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(kvmux_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

add_executable(kvmux src/app/main.cpp)
target_link_libraries(kvmux PRIVATE kvmux::core)

if(BUILD_TESTING)
    add_executable(kvmux_build_info_test tests/build_info_test.cpp)
    target_link_libraries(kvmux_build_info_test PRIVATE kvmux::core)
    add_test(NAME build_info COMMAND kvmux_build_info_test)

    add_executable(kvmux_ch9329_control_test tests/ch9329_control_test.cpp)
    target_link_libraries(kvmux_ch9329_control_test PRIVATE kvmux::core)
    add_test(NAME ch9329_control COMMAND kvmux_ch9329_control_test)

    add_executable(kvmux_video_contract_test tests/video_contract_test.cpp)
    target_link_libraries(kvmux_video_contract_test PRIVATE kvmux::core)
    add_test(NAME video_contract COMMAND kvmux_video_contract_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/red16.jpg")
endif()
