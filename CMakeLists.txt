cmake_minimum_required(VERSION 3.25)

if(APPLE)
    project(KVMux VERSION 0.1.0 LANGUAGES C CXX OBJCXX)
else()
    project(KVMux VERSION 0.1.0 LANGUAGES C CXX)
endif()

include(CTest)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(FFmpeg REQUIRED)
find_package(OpenGL REQUIRED)
add_subdirectory(third_party)

option(KVMUX_ENABLE_SANITIZERS "Enable the supported address and undefined behavior sanitizers" OFF)

add_library(kvmux_core STATIC
    src/control/ch9329_protocol.cpp
    src/control/control_queue.cpp
    src/control/hid_keyboard.cpp
    src/control/serial_transport.cpp
    src/control/serial_worker.cpp
    src/support/build_info.cpp
    src/video/capture_sample.cpp
    src/video/video_processor.cpp
)
add_library(kvmux::core ALIAS kvmux_core)

add_library(kvmux_capture STATIC)
add_library(kvmux::capture ALIAS kvmux_capture)
target_compile_features(kvmux_capture PUBLIC cxx_std_20)
target_include_directories(kvmux_capture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(kvmux_capture PUBLIC kvmux::core)
if(WIN32)
    target_sources(kvmux_capture PRIVATE src/video/capture/capture_media_foundation.cpp)
    target_link_libraries(kvmux_capture PRIVATE mfplat mfreadwrite mfuuid ole32)
elseif(APPLE)
    target_sources(kvmux_capture PRIVATE src/video/capture/capture_avfoundation.mm)
    target_compile_options(kvmux_capture PRIVATE -fobjc-arc)
    target_link_libraries(kvmux_capture PRIVATE
        "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(kvmux_capture PRIVATE src/video/capture/capture_v4l2.cpp)
else()
    message(FATAL_ERROR "KVMux capture supports Windows, Linux and macOS")
endif()

add_library(kvmux_renderer STATIC src/render/video_renderer.cpp)
add_library(kvmux::renderer ALIAS kvmux_renderer)
target_compile_features(kvmux_renderer PUBLIC cxx_std_20)
target_include_directories(kvmux_renderer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(kvmux_renderer PUBLIC kvmux::core kvmux::glad OpenGL::GL)
target_compile_features(kvmux_core PUBLIC cxx_std_20)
target_include_directories(kvmux_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(kvmux_core PUBLIC FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale kvmux::libserialport)

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
target_link_libraries(kvmux PRIVATE kvmux::core kvmux::capture kvmux::renderer kvmux::imgui kvmux::glad SDL3::SDL3 OpenGL::GL)

if(BUILD_TESTING)
    add_executable(kvmux_build_info_test tests/build_info_test.cpp)
    target_link_libraries(kvmux_build_info_test PRIVATE kvmux::core)
    add_test(NAME build_info COMMAND kvmux_build_info_test)

    add_executable(kvmux_ch9329_control_test tests/ch9329_control_test.cpp)
    target_link_libraries(kvmux_ch9329_control_test PRIVATE kvmux::core)
    add_test(NAME ch9329_control COMMAND kvmux_ch9329_control_test)

    add_executable(kvmux_serial_worker_test tests/serial_worker_test.cpp)
    target_link_libraries(kvmux_serial_worker_test PRIVATE kvmux::core)
    add_test(NAME serial_worker COMMAND kvmux_serial_worker_test)

    add_executable(kvmux_video_contract_test tests/video_contract_test.cpp)
    target_link_libraries(kvmux_video_contract_test PRIVATE kvmux::core)
    add_test(NAME video_contract COMMAND kvmux_video_contract_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/red16.jpg")
endif()
