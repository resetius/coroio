find_package(PkgConfig REQUIRED)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
pkg_check_modules(URING REQUIRED liburing)
endif ()
pkg_check_modules(OPENSSL openssl)

set(SOURCES
  address.cpp
  init.cpp
  socket.cpp
  sockutils.cpp
  poll.cpp
  select.cpp
  epoll.cpp
  uring.cpp
  kqueue.cpp
  resolver.cpp
  ssl.cpp
  iocp.cpp
  ws.cpp
  win32_pipe.cpp
)

if (WIN32)
  list(APPEND SOURCES
    wepoll.c
  )
  set_source_files_properties(wepoll.c PROPERTIES COMPILE_FLAGS "-w")
endif ()

add_library(coroio ${SOURCES})

target_include_directories(coroio PUBLIC ${URING_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_directories(coroio PUBLIC ${URING_LIBRARY_DIRS} ${OPENSSL_LIBRARY_DIRS})
target_link_libraries(coroio PUBLIC ${URING_LIBRARIES} ${OPENSSL_LIBRARIES})
if (WIN32)
    target_link_libraries(coroio PUBLIC ws2_32)
endif()

if(MSVC)
    target_compile_options(coroio PRIVATE /W4 /WX)
else()
    target_compile_options(coroio PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

target_compile_features(coroio PUBLIC cxx_std_20)
