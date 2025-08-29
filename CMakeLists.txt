cmake_minimum_required(VERSION 3.22)
if (EXISTS ${CMAKE_BINARY_DIR}/local.cmake)
include(${CMAKE_BINARY_DIR}/local.cmake)
endif ()

project(netlib)
enable_testing()

add_subdirectory(coroio)
add_subdirectory(coroio/dns)
add_subdirectory(coroio/actors)
add_subdirectory(examples)
add_subdirectory(tests)
