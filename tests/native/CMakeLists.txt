find_package(Threads REQUIRED)
add_executable(klogg_reliability_probe "${CMAKE_CURRENT_LIST_DIR}/reliability_probe.cpp")
target_link_libraries(klogg_reliability_probe PRIVATE
  klogg_logdata klogg_logging Qt${QT_VERSION_MAJOR}::Widgets Threads::Threads)
set_target_properties(klogg_reliability_probe PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/reliability-probe")
if(WIN32)
  target_link_libraries(klogg_reliability_probe PRIVATE psapi shell32)
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/src/logdata/include/longlinesearch.h")
  target_compile_definitions(klogg_reliability_probe PRIVATE KLOGG_LONG_LINE_CANDIDATE=1)
  set(empty_search_fixture "${CMAKE_BINARY_DIR}/reliability-probe/empty-search-fixture.log")
  file(WRITE "${empty_search_fixture}" "")
  add_test(NAME klogg_empty_search_probe COMMAND klogg_reliability_probe
    search "${empty_search_fixture}" "KLOGG_EMPTY_PROBE" --timeout-ms 15000)
  set_tests_properties(klogg_empty_search_probe PROPERTIES TIMEOUT 30)
  add_executable(klogg_worker_lifecycle_tests "${CMAKE_CURRENT_LIST_DIR}/worker_lifecycle_tests.cpp")
  target_link_libraries(klogg_worker_lifecycle_tests PRIVATE Catch2 klogg_logdata klogg_logging
    Qt${QT_VERSION_MAJOR}::Widgets Threads::Threads)
  set_target_properties(klogg_worker_lifecycle_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/reliability-probe")
  add_test(NAME klogg_worker_lifecycle_tests COMMAND klogg_worker_lifecycle_tests)
  set_tests_properties(klogg_worker_lifecycle_tests PROPERTIES TIMEOUT 60)
  if(WIN32)
    add_executable(klogg_efsw_lifecycle_tests "${CMAKE_CURRENT_LIST_DIR}/efsw_lifecycle_tests.cpp")
    target_link_libraries(klogg_efsw_lifecycle_tests PRIVATE Catch2 efsw)
    target_include_directories(klogg_efsw_lifecycle_tests BEFORE PRIVATE
      "${CMAKE_SOURCE_DIR}/3rdparty/efsw-win32" "${efsw_SOURCE_DIR}/src")
    set_target_properties(klogg_efsw_lifecycle_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/reliability-probe")
    add_test(NAME klogg_efsw_lifecycle_tests COMMAND klogg_efsw_lifecycle_tests)
  endif()
  if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/literal_tests.cpp")
    add_executable(klogg_literal_tests "${CMAKE_CURRENT_LIST_DIR}/literal_tests.cpp")
    target_link_libraries(klogg_literal_tests PRIVATE Catch2 klogg_logdata klogg_logging
      Qt${QT_VERSION_MAJOR}::Widgets Threads::Threads)
    set_target_properties(klogg_literal_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/reliability-probe")
    add_test(NAME klogg_literal_tests COMMAND klogg_literal_tests)
  endif()
  if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/preview_tests.cpp")
    add_executable(klogg_preview_tests "${CMAKE_CURRENT_LIST_DIR}/preview_tests.cpp")
    target_link_libraries(klogg_preview_tests PRIVATE Catch2 klogg_ui klogg_logging
      Qt${QT_VERSION_MAJOR}::Widgets Threads::Threads)
    set_target_properties(klogg_preview_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/reliability-probe")
    add_test(NAME klogg_preview_tests COMMAND klogg_preview_tests)
  endif()
  if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/crawler_tests.cpp")
    add_executable(klogg_crawler_tests "${CMAKE_CURRENT_LIST_DIR}/crawler_tests.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/queued_ui_lifecycle_tests.cpp")
    target_link_libraries(klogg_crawler_tests PRIVATE Catch2 klogg_ui klogg_logging
      Qt${QT_VERSION_MAJOR}::Widgets Threads::Threads)
    # Dialog headers inherit their generated Ui classes; expose them only to this test.
    get_target_property(klogg_ui_binary_dir klogg_ui BINARY_DIR)
    target_include_directories(klogg_crawler_tests PRIVATE
      "${klogg_ui_binary_dir}/klogg_ui_autogen/include")
    add_dependencies(klogg_crawler_tests klogg_ui_autogen)
    set_target_properties(klogg_crawler_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/reliability-probe")
    add_test(NAME klogg_crawler_tests COMMAND klogg_crawler_tests)
  endif()
endif()

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/gui_probe.cpp")
  add_executable(klogg_gui_probe "${CMAKE_CURRENT_LIST_DIR}/gui_probe.cpp")
  target_link_libraries(klogg_gui_probe PRIVATE klogg_ui klogg_logging Threads::Threads)
  set_target_properties(klogg_gui_probe PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/reliability-probe")
  if(WIN32)
    target_link_libraries(klogg_gui_probe PRIVATE psapi)
  endif()
endif()
