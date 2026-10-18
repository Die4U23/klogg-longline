add_library(
  klogg_filewatch STATIC ${CMAKE_CURRENT_SOURCE_DIR}/include/filewatcher.h
                         ${CMAKE_CURRENT_SOURCE_DIR}/src/filewatcher.cpp
)

set_target_properties(klogg_filewatch PROPERTIES AUTOMOC ON)

target_include_directories(klogg_filewatch PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(
  klogg_filewatch
  PUBLIC klogg_settings
         klogg_utils
         kdtoolbox
         efsw
)

if(KLOGG_USE_LTO)
  set_property(TARGET klogg_filewatch PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
