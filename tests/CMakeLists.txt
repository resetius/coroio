find_package(PkgConfig REQUIRED)
pkg_check_modules(GTEST gtest REQUIRED)

macro(ut name source)
  add_executable(ut_${name} ${source})
  target_include_directories(ut_${name} PRIVATE ${GTEST_INCLUDE_DIRS})
  target_link_directories(ut_${name} PRIVATE ${GTEST_LIBRARY_DIRS})
  target_link_libraries(ut_${name} PRIVATE coroio_actors ${GTEST_LIBRARIES})

  add_test(NAME ${name} COMMAND ${CMAKE_CURRENT_BINARY_DIR}/ut_${name} --gtest_output=xml:${name}.xml)
endmacro()

ut(tests tests.cpp)
ut(test_actors test_actors.cpp)
ut(test_http test_http.cpp)
ut(test_pipe test_pipe.cpp)
find_package(Threads REQUIRED)
target_link_libraries(ut_tests PRIVATE Threads::Threads)
target_link_libraries(ut_test_actors PRIVATE Threads::Threads)
