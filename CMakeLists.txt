cmake_minimum_required(VERSION 3.22)
if (EXISTS ${CMAKE_BINARY_DIR}/local.cmake)
include(${CMAKE_BINARY_DIR}/local.cmake)
endif ()

project(netlib)
find_package(PkgConfig REQUIRED)
pkg_check_modules(CMOCKA REQUIRED cmocka)
pkg_check_modules(URING liburing)
enable_testing()

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

macro(target name source)
  add_executable(${name} ${source})
  target_include_directories(${name} PRIVATE ${CMOCKA_INCLUDE_DIRS} ${URING_INCLUDE_DIRS})
  target_link_directories(${name} PRIVATE ${CMOCKA_LIBRARY_DIRS} ${URING_LIBRARY_DIRS})
  target_link_libraries(${name} ${CMOCKA_LIBRARIES} ${URING_LIBRARIES})
endmacro()

macro(ut name source)
  add_executable(ut_${name} ${source})
  target_include_directories(ut_${name} PRIVATE ${CMOCKA_INCLUDE_DIRS} ${URING_INCLUDE_DIRS})
  target_link_directories(ut_${name} PRIVATE ${CMOCKA_LIBRARY_DIRS} ${URING_LIBRARY_DIRS})
  target_link_libraries(ut_${name} ${CMOCKA_LIBRARIES} ${URING_LIBRARIES})

  add_test(NAME ${name} COMMAND ${CMAKE_CURRENT_BINARY_DIR}/ut_${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "CMOCKA_MESSAGE_OUTPUT=xml;CMOCKA_XML_FILE=${name}.xml")
endmacro()

target(timers examples/timers.cpp)
target(echotest examples/echotest.cpp)
target(echoserver examples/echoserver.cpp)
target(echoclient examples/echoclient.cpp)
target(bench examples/bench.cpp)

ut(tests tests/tests.cpp)
