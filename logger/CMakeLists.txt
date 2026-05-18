cmake_minimum_required(VERSION 3.20)

project(logger)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

add_library(logger SHARED logger.cpp logger.h)

target_include_directories(logger PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
