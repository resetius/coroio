find_package(PkgConfig REQUIRED)
pkg_check_modules(CMOCKA cmocka)

macro(ut name source)
  add_executable(ut_${name} ${source})
  target_include_directories(ut_${name} PRIVATE ${CMOCKA_INCLUDE_DIRS})
  target_link_directories(ut_${name} PRIVATE ${CMOCKA_LIBRARY_DIRS})
  target_link_libraries(ut_${name} PRIVATE coroio ${CMOCKA_LIBRARIES})

  add_test(NAME ${name} COMMAND ${CMAKE_CURRENT_BINARY_DIR}/ut_${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "CMOCKA_MESSAGE_OUTPUT=xml;CMOCKA_XML_FILE=${name}.xml")
endmacro()

if (CMOCKA_FOUND)
ut(tests tests.cpp)
endif ()
