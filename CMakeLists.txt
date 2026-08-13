cmake_minimum_required(VERSION 3.16)
project(drone_codec_motion LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)

add_executable(affine_test affine_test.cpp)
target_link_libraries(affine_test PRIVATE ${OpenCV_LIBS})

add_executable(mesh_test mesh_test.cpp)
target_link_libraries(mesh_test PRIVATE ${OpenCV_LIBS})

add_executable(keyframe_test keyframe_test.cpp)
target_link_libraries(keyframe_test PRIVATE ${OpenCV_LIBS})

# Keep executable paths stable across single- and multi-config generators.
foreach(target affine_test mesh_test keyframe_test)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin")
endforeach()
