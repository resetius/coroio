cmake_minimum_required(VERSION 3.22)
if (EXISTS ${CMAKE_BINARY_DIR}/local.cmake)
include(${CMAKE_BINARY_DIR}/local.cmake)
endif ()

project(netlib)
find_package(PkgConfig REQUIRED)
pkg_check_modules(CMOCKA REQUIRED cmocka)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
pkg_check_modules(URING REQUIRED liburing)
endif ()
pkg_check_modules(OPENSSL openssl)
enable_testing()

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(coroio
  coroio/socket.cpp
  coroio/sockutils.cpp
  coroio/poll.cpp
  coroio/select.cpp
  coroio/epoll.cpp
  coroio/uring.cpp
  coroio/kqueue.cpp
  coroio/resolver.cpp
  coroio/ssl.cpp
  )

target_include_directories(coroio PUBLIC ${URING_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIRS})
target_link_directories(coroio PUBLIC ${URING_LIBRARY_DIRS} ${OPENSSL_LIBRARY_DIRS})
target_link_libraries(coroio PUBLIC ${URING_LIBRARIES} ${OPENSSL_LIBRARIES})

if(MSVC)
    target_compile_options(coroio PRIVATE /W4 /WX)
else()
    target_compile_options(coroio PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

macro(target name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} coroio)
endmacro()

macro(ut name source)
  add_executable(ut_${name} ${source})
  target_include_directories(ut_${name} PRIVATE ${CMOCKA_INCLUDE_DIRS})
  target_link_directories(ut_${name} PRIVATE ${CMOCKA_LIBRARY_DIRS})
  target_link_libraries(ut_${name} PRIVATE coroio ${CMOCKA_LIBRARIES})

  add_test(NAME ${name} COMMAND ${CMAKE_CURRENT_BINARY_DIR}/ut_${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "CMOCKA_MESSAGE_OUTPUT=xml;CMOCKA_XML_FILE=${name}.xml")
endmacro()

target(timers examples/timers.cpp)
target(echotest examples/echotest.cpp)
target(echoserver examples/echoserver.cpp)
target(echoclient examples/echoclient.cpp)
target(sslechoclient examples/sslechoclient.cpp)
target(sslechoserver examples/sslechoserver.cpp)
target(resolver examples/resolver.cpp)
target(bench examples/bench.cpp)

ut(tests tests/tests.cpp)
