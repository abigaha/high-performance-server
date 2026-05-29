cmake_minimum_required(VERSION 3.20)

project(coroutine)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)

add_library(${PROJECT_NAME} INTERFACE)

target_include_directories(
  ${PROJECT_NAME} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include
                            ${CMAKE_SOURCE_DIR}/memory-pool/include)

target_sources(${PROJECT_NAME}
               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/coroitem.hpp)

target_link_directories(${PROJECT_NAME} INTERFACE ${CMAKE_SOURCE_DIR}/lib)

target_link_libraries(${PROJECT_NAME} INTERFACE logger memory-pool)

target_compile_options(${PROJECT_NAME} INTERFACE -fopenmp -ltbb)
