# Retain the pinned dependency; replace only its Windows backend in this target.
if(NOT WIN32)
  return()
endif()

file(SHA256 "${efsw_SOURCE_DIR}/src/efsw/FileWatcherWin32.cpp" original_sha)
if(NOT original_sha STREQUAL "79fecd4c9e97476abbcce55ca008bedbd0fae084199911cd8c2d093304c4ac34")
  message(FATAL_ERROR "efsw 1.4.1 source mismatch: FileWatcherWin32.cpp; review overlay before changing dependency")
endif()
file(SHA256 "${efsw_SOURCE_DIR}/src/efsw/FileWatcherWin32.hpp" original_sha)
if(NOT original_sha STREQUAL "0acd0f7f62bd06f9b5a5c09af563e8a4ef749e9be467c234a8a7eb1568f26a19")
  message(FATAL_ERROR "efsw 1.4.1 source mismatch: FileWatcherWin32.hpp; review overlay before changing dependency")
endif()
file(SHA256 "${efsw_SOURCE_DIR}/src/efsw/WatcherWin32.cpp" original_sha)
if(NOT original_sha STREQUAL "b82cfeda318fa1c805837b2c2e854f23d5670f9e6aafaff3df5723234e9398e6")
  message(FATAL_ERROR "efsw 1.4.1 source mismatch: WatcherWin32.cpp; review overlay before changing dependency")
endif()
file(SHA256 "${efsw_SOURCE_DIR}/src/efsw/WatcherWin32.hpp" original_sha)
if(NOT original_sha STREQUAL "54bf2434ba4197ed010ea98e0768cfb2e7dea327683b84ace7b208f2c10fd141")
  message(FATAL_ERROR "efsw 1.4.1 source mismatch: WatcherWin32.hpp; review overlay before changing dependency")
endif()

get_target_property(efsw_sources efsw SOURCES)
list(FILTER efsw_sources EXCLUDE REGEX "(^|/)(FileWatcherWin32|WatcherWin32)\\.cpp$")
set_property(TARGET efsw PROPERTY SOURCES "${efsw_sources}")
target_sources(efsw PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/efsw/FileWatcherWin32.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/efsw/WatcherWin32.cpp")
target_include_directories(efsw BEFORE PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
