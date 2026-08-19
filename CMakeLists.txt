cmake_minimum_required(VERSION 3.16)
project(flowx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)
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
add_library(flowx_codec udp_image_codec.cpp motion_refine.cpp)
add_library(udp_image_codec ALIAS flowx_codec)
target_include_directories(flowx_codec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(flowx_codec PUBLIC ${OpenCV_LIBS})

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

# Sender-side runtime: image acquisition, product-level patch shaping, and FlowX
# UDP transmission. HTTP image acquisition intentionally uses cpp-httplib; raw
# sockets are used only for UDP.
add_library(flowx_sender_runtime
    flowx_image_source.cpp
    flowx_net.cpp
    flowx_patch_rewrite.cpp)
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
add_library(flowx_receiver_runtime
    flowx_udp_receiver.cpp
    flowx_frame_store.cpp
    flowx_raw_store.cpp
    flowx_browser_v4_assets.cpp
    flowx_http_server.cpp)
target_include_directories(flowx_receiver_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
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

# Keep executable paths stable across single- and multi-config generators.
foreach(target affine_test mesh_test keyframe_test codec_test flowx_sender flowx_receiver)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin")
endforeach()
