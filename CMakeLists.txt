cmake_minimum_required(VERSION 3.22)
if (EXISTS ${CMAKE_BINARY_DIR}/local.cmake)
    include(${CMAKE_BINARY_DIR}/local.cmake)
endif ()

project(netlib)

set(_COROIO_IS_ROOT OFF)
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_COROIO_IS_ROOT ON)
endif()

option(COROIO_BUILD_EXAMPLES "Build coroio examples" ${_COROIO_IS_ROOT})
option(COROIO_BUILD_TESTS "Build coroio tests" ${_COROIO_IS_ROOT})

add_subdirectory(coroio)
add_subdirectory(coroio/dns)
add_subdirectory(coroio/actors)

if (COROIO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if (COROIO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
