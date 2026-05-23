cmake_minimum_required(VERSION 3.20)

project(logger)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)

add_library(logger SHARED)

target_include_directories(logger PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_sources(
  logger
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/logger.h
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/logformatter.h
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/logappender.h)

target_sources(
  logger
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/logger.cpp
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/logformatter.cpp
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/logappender.cpp)
