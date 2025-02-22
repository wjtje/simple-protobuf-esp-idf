cmake_minimum_required( VERSION 3.12 )

project(spb-proto VERSION 1.0.0 LANGUAGES CXX)

# Set the C++ standard to C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# the main serialization library is header only
add_library(spb-proto INTERFACE)
target_include_directories(spb-proto INTERFACE ../include)
target_compile_features(spb-proto INTERFACE cxx_std_20)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/spb_warnings.cmake)

add_subdirectory(spb-proto-compiler)
