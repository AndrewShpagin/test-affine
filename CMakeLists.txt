cmake_minimum_required(VERSION 3.16)
project(flowx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # Embedded browser assets are UTF-8, independently of the Windows code page.
    add_compile_options(/utf-8)
endif()

find_package(OpenCV REQUIRED)
find_package(JPEG REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)

# cpp-httplib: prefer its CMake package when available; Debian packages also
# provide a pkg-config module.
find_package(httplib CONFIG QUIET)
if(TARGET httplib::httplib)
    set(FLOWX_HTTPLIB_TARGET httplib::httplib)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CPPHTTPLIB REQUIRED IMPORTED_TARGET cpp-httplib)
    set(FLOWX_HTTPLIB_TARGET PkgConfig::CPPHTTPLIB)
endif()

add_executable(affine_test affine_test.cpp)
target_link_libraries(affine_test PRIVATE ${OpenCV_LIBS})

add_executable(mesh_test mesh_test.cpp)
target_link_libraries(mesh_test PRIVATE ${OpenCV_LIBS})

add_executable(keyframe_test keyframe_test.cpp)
target_link_libraries(keyframe_test PRIVATE ${OpenCV_LIBS})

# Stable codec implementation. Keep the historical target name as an alias while
# product-facing code uses FlowX naming.
add_library(flowx_codec udp_image_codec.cpp motion_refine.cpp jpeg_restart.cpp jpeg_restart_assembly.cpp jpeg_restart_fill.cpp)
add_library(udp_image_codec ALIAS flowx_codec)
target_include_directories(flowx_codec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(flowx_codec PUBLIC ${OpenCV_LIBS} JPEG::JPEG)

add_executable(codec_test codec_test.cpp)
target_link_libraries(codec_test PRIVATE flowx_codec)

# The stable codec still speaks AFC1 internally. flowx_protocol_v4.cpp compacts
# AFC1 to the network wire format and reconstructs AFC1 for the C++ decoder.
add_library(flowx_common
    flowx_protocol_v4.cpp
    flowx_config.cpp)
target_include_directories(flowx_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(flowx_common
    PUBLIC
        flowx_codec
        nlohmann_json::nlohmann_json)

add_executable(flowx_protocol_test flowx_protocol_test.cpp)
target_link_libraries(flowx_protocol_test PRIVATE flowx_common)

# Sender-side runtime: image acquisition, product-level patch shaping, and FlowX
# UDP transmission. HTTP image acquisition intentionally uses cpp-httplib; raw
# sockets are used only for UDP.
add_library(flowx_sender_runtime
    flowx_image_source.cpp
    flowx_net.cpp
    flowx_patch_rewrite.cpp
    flowx_sender_http.cpp)
target_include_directories(flowx_sender_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(flowx_sender_runtime
    PUBLIC
        flowx_common
        ${FLOWX_HTTPLIB_TARGET}
        Threads::Threads)
if(WIN32)
    target_link_libraries(flowx_sender_runtime PUBLIC ws2_32)
endif()

# Receiver runtime: UDP input, decoded-frame publication, browser FlowX transport,
# and HTTP image/status/browser output.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS flowx_restart_browser.js)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/flowx_restart_browser.js" FLOWX_RESTART_BROWSER_SOURCE)
configure_file(flowx_restart_browser_source.h.in flowx_restart_browser_source.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS flowx_browser_playout.js)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/flowx_browser_playout.js" FLOWX_BROWSER_PLAYOUT_SOURCE)
configure_file(flowx_browser_playout_source.h.in flowx_browser_playout_source.h @ONLY)
add_library(flowx_receiver_runtime
    flowx_udp_receiver.cpp
    flowx_packet_jitter.cpp
    flowx_frame_store.cpp
    flowx_raw_store.cpp
    flowx_browser_v4_assets.cpp
    flowx_http_server.cpp)
target_include_directories(flowx_receiver_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(flowx_receiver_runtime PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(flowx_receiver_runtime
    PUBLIC
        flowx_common
        ${FLOWX_HTTPLIB_TARGET}
        Threads::Threads)
if(WIN32)
    target_link_libraries(flowx_receiver_runtime PUBLIC ws2_32)
endif()

add_executable(flowx_sender flowx_sender.cpp)
target_link_libraries(flowx_sender PRIVATE flowx_sender_runtime)

add_executable(flowx_receiver flowx_receiver.cpp)
target_link_libraries(flowx_receiver PRIVATE flowx_receiver_runtime)

include(CTest)
if(BUILD_TESTING)
    add_executable(flowx_packet_jitter_test flowx_packet_jitter_test.cpp flowx_packet_jitter.cpp)
    add_test(NAME flowx_packet_jitter COMMAND flowx_packet_jitter_test)
    add_executable(jpeg_restart_test jpeg_restart_test.cpp)
    target_link_libraries(jpeg_restart_test PRIVATE flowx_receiver_runtime)
    add_test(NAME flowx_protocol COMMAND flowx_protocol_test)
    add_test(NAME jpeg_restart COMMAND jpeg_restart_test)
    find_program(FLOWX_NODE_EXECUTABLE NAMES node nodejs)
    if(FLOWX_NODE_EXECUTABLE)
        add_test(NAME flowx_browser_playout COMMAND "${FLOWX_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/flowx_browser_playout_test.js")
        add_test(NAME flowx_nearest_fill COMMAND "${FLOWX_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/flowx_nearest_fill_test.js")
        add_test(NAME flowx_smooth_fill COMMAND "${FLOWX_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/flowx_smooth_fill_test.js" --export
            "${CMAKE_CURRENT_BINARY_DIR}/smooth-fill-cases.json")
        set_tests_properties(flowx_smooth_fill PROPERTIES FIXTURES_SETUP smooth_fill_cases)
        add_executable(jpeg_restart_fill_test jpeg_restart_fill_test.cpp)
        target_link_libraries(jpeg_restart_fill_test PRIVATE flowx_common)
        add_test(NAME jpeg_restart_fill COMMAND jpeg_restart_fill_test
            "${CMAKE_CURRENT_BINARY_DIR}/smooth-fill-cases.json")
        set_tests_properties(jpeg_restart_fill PROPERTIES FIXTURES_REQUIRED smooth_fill_cases)
        add_test(NAME jpeg_restart_fixtures COMMAND jpeg_restart_test --export
            "${CMAKE_CURRENT_BINARY_DIR}/restart-fixtures.json")
        set_tests_properties(jpeg_restart_fixtures PROPERTIES FIXTURES_SETUP restart_fixtures)
        add_test(NAME jpeg_restart_browser COMMAND "${FLOWX_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/flowx_restart_browser_test.js"
            "${CMAKE_CURRENT_BINARY_DIR}/restart-fixtures.json")
        set_tests_properties(jpeg_restart_browser PROPERTIES FIXTURES_REQUIRED restart_fixtures)
        add_test(NAME flowx_browser_presentation COMMAND "${FLOWX_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/flowx_browser_presentation_test.js"
            "${CMAKE_CURRENT_BINARY_DIR}/restart-fixtures.json")
        set_tests_properties(flowx_browser_presentation PROPERTIES FIXTURES_REQUIRED restart_fixtures)
        add_test(NAME jpeg_shuffle_fixtures COMMAND jpeg_restart_test --export-shuffled
            "${CMAKE_CURRENT_BINARY_DIR}/shuffle-fixtures.json")
        set_tests_properties(jpeg_shuffle_fixtures PROPERTIES FIXTURES_SETUP shuffle_fixtures)
        add_test(NAME jpeg_shuffle_browser COMMAND "${FLOWX_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/flowx_restart_browser_test.js"
            "${CMAKE_CURRENT_BINARY_DIR}/shuffle-fixtures.json")
        add_test(NAME jpeg_shuffle_presentation COMMAND "${FLOWX_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/flowx_browser_presentation_test.js"
            "${CMAKE_CURRENT_BINARY_DIR}/shuffle-fixtures.json")
        set_tests_properties(jpeg_shuffle_browser jpeg_shuffle_presentation
            PROPERTIES FIXTURES_REQUIRED shuffle_fixtures)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME flowx_mjpeg COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/flowx_mjpeg_test.py" "$<TARGET_FILE:flowx_receiver>"
                "${CMAKE_CURRENT_BINARY_DIR}/restart-fixtures.json")
            add_test(NAME flowx_mjpeg_shuffle COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/flowx_mjpeg_test.py" "$<TARGET_FILE:flowx_receiver>"
                "${CMAKE_CURRENT_BINARY_DIR}/shuffle-fixtures.json")
            set_tests_properties(flowx_mjpeg PROPERTIES FIXTURES_REQUIRED restart_fixtures TIMEOUT 45)
            set_tests_properties(flowx_mjpeg_shuffle PROPERTIES FIXTURES_REQUIRED shuffle_fixtures TIMEOUT 45)
        endif()
    endif()
endif()

# Keep executable paths stable across single- and multi-config generators.
foreach(target affine_test mesh_test keyframe_test codec_test flowx_protocol_test flowx_sender flowx_receiver)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin")
endforeach()

# vcpkg copies dependent DLLs beside the executable during the build. Include
# those and the MSVC runtime in a relocatable Windows sender/receiver package.
if(WIN32)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ".")
    include(InstallRequiredSystemLibraries)
    install(TARGETS flowx_receiver flowx_sender RUNTIME DESTINATION ".")
    install(DIRECTORY "${CMAKE_BINARY_DIR}/bin/" DESTINATION "."
        FILES_MATCHING PATTERN "*.dll")
    install(FILES config/flowx_receiver.json config/flowx_sender_windows.json
        DESTINATION config)
    install(FILES packaging/windows/run-receiver.cmd packaging/windows/run-sender.cmd
        DESTINATION ".")
    install(FILES README_WINDOWS.md DESTINATION "." RENAME README.md)
endif()
