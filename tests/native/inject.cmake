# Use -DCMAKE_PROJECT_klogg_INCLUDE=<absolute path to this file>.
# Adds only an external diagnostic target after pristine upstream defines its libraries.
include("${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/msvc-dependencies.cmake")
if(CMAKE_VERSION VERSION_LESS 3.19)
  message(FATAL_ERROR "Reliability probe injection requires CMake 3.19 or newer for DEFER")
endif()
if(NOT DEFINED KLOGG_RELIABILITY_PROBE_SCHEDULED)
  set(KLOGG_RELIABILITY_PROBE_SCHEDULED TRUE)
  function(klogg_add_reliability_probe)
    include("${KLOGG_RELIABILITY_PROBE_SOURCE_DIR}/CMakeLists.txt")
  endfunction()
  set(KLOGG_RELIABILITY_PROBE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
  cmake_language(DEFER CALL klogg_add_reliability_probe)
endif()
