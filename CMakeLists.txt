cmake_minimum_required(VERSION 3.20)

project(high-performance-server)

set(CMAKE_CXX_STANDARD 23)

set(CMAKE_BUILD_TYPE Debug)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(${CMAKE_SOURCE_DIR}/core/logger)

add_subdirectory(${CMAKE_SOURCE_DIR}/memory-pool)

add_subdirectory(${CMAKE_SOURCE_DIR}/coroutine)

add_subdirectory(${CMAKE_SOURCE_DIR}/core)

# target_compile_definitions( high-performance-server PRIVATE
# $<CONFIG:DEBUG>:DEBUG_BUILD $<CONFIG:RELEASE>:RELEASE_BUILD)
