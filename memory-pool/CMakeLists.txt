cmake_minimum_required(VERSION 3.20)

project(memory-pool)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)

add_library(${PROJECT_NAME} SHARED)

target_include_directories(
  ${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                         ${CMAKE_SOURCE_DIR}/core/logger/include)

target_sources(${PROJECT_NAME}
               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/memory_pool.h)

target_sources(${PROJECT_NAME}
               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/memory_pool.cpp)
# target_sources(${PROJECT_NAME} PRIVATE
# ${CMAKE_CURRENT_SOURCE_DIR}/src/coroitem.cpp)

target_link_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/lib)

target_link_libraries(${PROJECT_NAME} PRIVATE logger)
