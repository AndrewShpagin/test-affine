cmake_minimum_required(VERSION 3.16)
project(flowx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)
find_package(nlohmann_json REQUIRED)

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

# Product-level transport/config foundation. Networking and image sources are
# intentionally added in subsequent PRs.
add_library(flowx_common
    flowx_protocol.cpp
    flowx_config.cpp)
target_include_directories(flowx_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(flowx_common
    PUBLIC
        flowx_codec
        nlohmann_json::nlohmann_json)

add_executable(flowx_sender flowx_sender.cpp)
target_link_libraries(flowx_sender PRIVATE flowx_common)

add_executable(flowx_receiver flowx_receiver.cpp)
target_link_libraries(flowx_receiver PRIVATE flowx_common)

# Keep executable paths stable across single- and multi-config generators.
foreach(target affine_test mesh_test keyframe_test codec_test flowx_sender flowx_receiver)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin")
endforeach()
