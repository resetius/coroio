find_package(PkgConfig REQUIRED)

pkg_check_modules(URING liburing)
pkg_check_modules(OPENSSL openssl)

set(SOURCES
  address.cpp
  init.cpp
  socket.cpp
  sockutils.cpp
  ssl.cpp
  win32_pipe.cpp
  backends/poll.cpp
  backends/select.cpp
  backends/epoll.cpp
  backends/uring.cpp
  backends/kqueue.cpp
  backends/iocp.cpp
  ws/ws.cpp
  ws/utils.cpp
  http/httpd.cpp
  pipe/pipe.cpp
)

if (WIN32)
  list(APPEND SOURCES
    backends/wepoll.c
  )
  set_source_files_properties(backends/wepoll.c PROPERTIES COMPILE_FLAGS "-w")
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

target_compile_features(coroio PUBLIC cxx_std_23)
