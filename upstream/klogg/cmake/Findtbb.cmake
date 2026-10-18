# - Find tbb
# Find the native tbb headers and libraries.
#
# TBB_INCLUDE_DIRS	- where to find tbb.h, etc.
# TBB_LIBRARIES	- List of libraries when using tbb.
# TBB_FOUND	- True if tbb found.

# Look for the header file.
find_path(
  TBB_INCLUDE_DIR
  NAMES tbb.h
  PATH_SUFFIXES tbb
)

# Look for the library.
find_library(TBB_LIBRARY NAMES tbb)

# Handle the QUIETLY and REQUIRED arguments and set UCHARDET_FOUND to TRUE if all listed variables are TRUE.
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  tbb
  DEFAULT_MSG
  TBB_LIBRARY
  TBB_INCLUDE_DIR
)

# Copy the results to the output variables.
if(TBB_FOUND)
  set(TBB_LIBRARIES ${TBB_LIBRARY})
  set(TBB_INCLUDE_DIRS ${TBB_INCLUDE_DIR})
else(TBB_FOUND)
  set(TBB_LIBRARIES)
  set(TBB_INCLUDE_DIRS)
endif(TBB_FOUND)

mark_as_advanced(TBB_INCLUDE_DIRS TBB_LIBRARIES)
