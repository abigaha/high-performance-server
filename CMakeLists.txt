cmake_minimum_required(VERSION 3.20)

project(high-performance-server)

set(CMAKE_CXX_STANDARD 23)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

set(CMAKE_BUILD_TYPE Debug)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(${CMAKE_SOURCE_DIR}/logger)

add_executable(${PROJECT_NAME})

target_include_directories(${PROJECT_NAME})

target_sources(${PROJECT_NAME})

target_link_libraries(${PROJECT_NAME} logger)

target_compile_options(${PROJECT_NAME})

target_link_options(${PROJECT_NAME})

target_precompile_headers(
  ${PROJECT_NAME}
  PRIVATE
  <iostream>
  <vector>
  <string>
  <thread>
  <mutex>
  <condition_variable>
  <chrono>
  <atomic>
  <memory>
  <unordered_map>
  <queue>
  <functional>
  <algorithm>)
