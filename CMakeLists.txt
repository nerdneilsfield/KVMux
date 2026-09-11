cmake_minimum_required(VERSION 3.25)

if(APPLE)
    project(KVMux VERSION 0.1.0 LANGUAGES C CXX OBJCXX)
else()
    project(KVMux VERSION 0.1.0 LANGUAGES C CXX)
endif()

include(CTest)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(FFmpeg REQUIRED)
option(KVMUX_BUILD_GUI "Build the desktop GUI and its display dependencies" ON)
if(KVMUX_BUILD_GUI)
    find_package(OpenGL REQUIRED)
endif()
add_subdirectory(third_party)

option(KVMUX_ENABLE_SANITIZERS "Enable the supported address and undefined behavior sanitizers" OFF)

add_library(kvmux_core STATIC
    src/control/ch9329_protocol.cpp
    src/control/control_queue.cpp
    src/control/hid_keyboard.cpp
    src/control/serial_transport.cpp
    src/control/serial_worker.cpp
    src/input/input_router.cpp
    src/network/relay_server.cpp
    src/network/relay_client.cpp
    src/network/relay_protocol.cpp
    src/network/tcp_socket.cpp
    src/support/build_info.cpp
    src/support/config.cpp
    src/support/diagnostics.cpp
    src/video/capture_sample.cpp
    src/video/video_processor.cpp
    src/video/video_pipeline.cpp
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
        "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo" "-framework Foundation")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(kvmux_capture PRIVATE src/video/capture/capture_v4l2.cpp)
else()
    message(FATAL_ERROR "KVMux capture supports Windows, Linux and macOS")
endif()

if(KVMUX_BUILD_GUI)
    add_library(kvmux_renderer STATIC src/render/video_renderer.cpp)
    add_library(kvmux::renderer ALIAS kvmux_renderer)
    target_compile_features(kvmux_renderer PUBLIC cxx_std_20)
    target_include_directories(kvmux_renderer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(kvmux_renderer PUBLIC kvmux::core kvmux::glad OpenGL::GL)
endif()
target_compile_features(kvmux_core PUBLIC cxx_std_20)
target_include_directories(kvmux_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(kvmux_core PUBLIC FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale kvmux::libserialport nlohmann_json::nlohmann_json spdlog::spdlog_header_only)

if(WIN32)
    target_link_libraries(kvmux_core PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_options(kvmux_core PRIVATE /W4 /permissive-)
    target_compile_options(kvmux_capture PRIVATE /W4 /permissive-)
    if(KVMUX_BUILD_GUI)
        target_compile_options(kvmux_renderer PRIVATE /W4 /permissive-)
    endif()
else()
    target_compile_options(kvmux_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    target_compile_options(kvmux_capture PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    if(KVMUX_BUILD_GUI)
        target_compile_options(kvmux_renderer PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    endif()
endif()

if(KVMUX_ENABLE_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "KVMUX_ENABLE_SANITIZERS is not supported by the MSVC preset")
    endif()
    target_compile_options(kvmux_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(kvmux_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

add_executable(kvmux-relay src/app/relay_main.cpp)
target_link_libraries(kvmux-relay PRIVATE kvmux::core kvmux::capture)

if(KVMUX_BUILD_GUI)
    add_executable(kvmux src/app/main.cpp src/app/kvm_session.cpp)
    if(APPLE)
        configure_file(packaging/macos/Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)
        set_target_properties(kvmux PROPERTIES MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
    endif()
    target_link_libraries(kvmux PRIVATE kvmux::core kvmux::capture kvmux::renderer kvmux::imgui kvmux::glad SDL3::SDL3 OpenGL::GL)
endif()

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

    add_executable(kvmux_input_router_test tests/input_router_test.cpp)
    target_link_libraries(kvmux_input_router_test PRIVATE kvmux::core)
    add_test(NAME input_router COMMAND kvmux_input_router_test)

    add_executable(kvmux_tcp_socket_test tests/tcp_socket_test.cpp)
    target_link_libraries(kvmux_tcp_socket_test PRIVATE kvmux::core)
    add_test(NAME tcp_socket COMMAND kvmux_tcp_socket_test)
    set_tests_properties(tcp_socket PROPERTIES TIMEOUT 15)

    add_executable(kvmux_relay_server_test tests/relay_server_test.cpp)
    target_link_libraries(kvmux_relay_server_test PRIVATE kvmux::core)
    add_test(NAME relay_server COMMAND kvmux_relay_server_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/red16.jpg")
    set_tests_properties(relay_server PROPERTIES TIMEOUT 15)

    add_executable(kvmux_relay_protocol_test tests/relay_protocol_test.cpp)
    target_link_libraries(kvmux_relay_protocol_test PRIVATE kvmux::core)
    add_test(NAME relay_protocol COMMAND kvmux_relay_protocol_test)

    add_executable(kvmux_relay_client_test tests/relay_client_test.cpp)
    target_link_libraries(kvmux_relay_client_test PRIVATE kvmux::core)
    add_test(NAME relay_client COMMAND kvmux_relay_client_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/red16.jpg")
    set_tests_properties(relay_client PROPERTIES TIMEOUT 15)

    add_executable(kvmux_video_contract_test tests/video_contract_test.cpp)
    target_link_libraries(kvmux_video_contract_test PRIVATE kvmux::core)
    add_test(NAME video_contract COMMAND kvmux_video_contract_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/red16.jpg")
endif()

include(GNUInstallDirs)
install(TARGETS kvmux-relay RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(KVMUX_BUILD_GUI)
    if(APPLE)
        set_target_properties(kvmux PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks")
        install(TARGETS kvmux RUNTIME_DEPENDENCY_SET kvmux_runtime BUNDLE DESTINATION .)
        install(RUNTIME_DEPENDENCY_SET kvmux_runtime
            DESTINATION "kvmux.app/Contents/Frameworks"
            PRE_EXCLUDE_REGEXES "/System/Library/.*" "/usr/lib/.*")
        install(FILES $<TARGET_FILE:SDL3::SDL3> DESTINATION "kvmux.app/Contents/Frameworks")
        install(CODE [[
            include(BundleUtilities)
            fixup_bundle("${CMAKE_INSTALL_PREFIX}/kvmux.app/Contents/MacOS/kvmux" "" "${CMAKE_INSTALL_PREFIX}/kvmux.app/Contents/Frameworks")
            execute_process(
                COMMAND codesign --force --deep --sign - "${CMAKE_INSTALL_PREFIX}/kvmux.app"
                COMMAND_ERROR_IS_FATAL ANY
            )
        ]])
    else()
        install(TARGETS kvmux RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    endif()
endif()
install(FILES third_party/INVENTORY.md DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/kvmux")
