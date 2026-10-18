cmake_minimum_required(VERSION 3.20)
project(klogg_index_probe LANGUAGES C CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/msvc-dependencies.cmake")

set(KLOGG_SOURCE_ROOT "" CACHE PATH "Pinned pristine klogg source root")
set(TYPE_SAFE_ROOT "" CACHE PATH "Cached type_safe source root")
set(STREAMVBYTE_ROOT "" CACHE PATH "streamvbyte 1.0.0 source root")
set(MIMALLOC_ROOT "" CACHE PATH "mimalloc 2.1.7 source root")
set(QT_ROOT "" CACHE PATH "Qt 6.7.3 MSVC x64 root")
foreach(required KLOGG_SOURCE_ROOT TYPE_SAFE_ROOT STREAMVBYTE_ROOT MIMALLOC_ROOT QT_ROOT)
  if(NOT IS_DIRECTORY "${${required}}")
    message(FATAL_ERROR "${required} is not an existing directory: ${${required}}")
  endif()
endforeach()

set(CMAKE_PREFIX_PATH "${QT_ROOT}" ${CMAKE_PREFIX_PATH})
find_package(Qt6 6.7.3 REQUIRED COMPONENTS Core)

set(TYPE_SAFE_ENABLE_ASSERTIONS OFF CACHE BOOL "" FORCE)
set(TYPE_SAFE_ENABLE_PRECONDITION_CHECKS ON CACHE BOOL "" FORCE)
add_subdirectory("${TYPE_SAFE_ROOT}" "${CMAKE_BINARY_DIR}/type_safe" EXCLUDE_FROM_ALL)
set(STREAMVBYTE_WALL OFF CACHE BOOL "" FORCE)
set(STREAMVBYTE_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(STREAMVBYTE_SANITIZE OFF CACHE BOOL "" FORCE)
add_subdirectory("${STREAMVBYTE_ROOT}" "${CMAKE_BINARY_DIR}/streamvbyte" EXCLUDE_FROM_ALL)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
set(MI_SECURE OFF CACHE BOOL "" FORCE)
add_subdirectory("${MIMALLOC_ROOT}" "${CMAKE_BINARY_DIR}/mimalloc" EXCLUDE_FROM_ALL)

add_executable(klogg_index_probe
  index_probe.cpp
  "${KLOGG_SOURCE_ROOT}/src/logdata/src/compressedlinestorage.cpp"
  "${KLOGG_SOURCE_ROOT}/src/utils/src/cpu_info.cpp"
  "${KLOGG_SOURCE_ROOT}/src/logging/src/logger.cpp"
)
target_compile_features(klogg_index_probe PRIVATE cxx_std_17)
target_include_directories(klogg_index_probe PRIVATE
  "${KLOGG_SOURCE_ROOT}/src/logdata/include"
  "${KLOGG_SOURCE_ROOT}/src/utils/include"
  "${KLOGG_SOURCE_ROOT}/src/logging/include"
)
target_link_libraries(klogg_index_probe PRIVATE
  Qt6::Core type_safe streamvbyte mimalloc-static
)
