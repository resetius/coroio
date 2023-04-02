cmake_minimum_required(VERSION 3.22)

project(netlib)
find_package(PkgConfig REQUIRED)
pkg_check_modules(CMOCKA REQUIRED cmocka)

include_directories(${CMAKE_SOURCE_DIR}/src)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(timers examples/timers.cpp)
add_executable(echotest examples/echotest.cpp)
add_executable(echoserver examples/echoserver.cpp)
add_executable(echoclient examples/echoclient.cpp)

add_executable(tests tests/tests.cpp)
target_include_directories(tests PRIVATE ${CMOCKA_INCLUDE_DIRS})
target_link_directories(tests PRIVATE ${CMOCKA_LIBRARY_DIRS})
target_link_libraries(tests ${CMOCKA_LIBRARIES})
