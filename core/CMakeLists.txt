cmake_minimum_required(VERSION 3.20)

project(high-performance-server)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/core/main.cpp)

target_link_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/lib)

target_link_libraries(${PROJECT_NAME} logger coroutine)

target_compile_options(
  ${PROJECT_NAME}
  PRIVATE -g
          -O0
          -Wall
          -Wextra
          -Wpedantic
          -Werror
          -fsanitize=address
          -fsanitize=undefined)

target_link_options(${PROJECT_NAME} PRIVATE -fsanitize=address
                    -fsanitize=undefined)

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
