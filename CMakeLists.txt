project(netlib)
cmake_minimum_required(VERSION 3.25)

include_directories(${CMAKE_SOURCE_DIR}/src)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(timers examples/timers.cpp)
