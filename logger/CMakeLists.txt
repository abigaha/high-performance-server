cmake_minimum_required(VERSION 3.20)

project(logger)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

add_library(logger SHARED logger.cpp logger.h)
