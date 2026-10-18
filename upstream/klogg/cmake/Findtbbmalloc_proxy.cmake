# - Find tbb
# Find the native tbb headers and libraries.
#
# TBB_INCLUDE_DIRS	- where to find tbb.h, etc.
# TBB_LIBRARIES	- List of libraries when using tbb.
# TBB_FOUND	- True if tbb found.

# Look for the header file.
find_path(
  TBB_MALLOC_PROXY_INCLUDE_DIR
  NAMES tbbmalloc_proxy.h
  PATH_SUFFIXES tbb
)

# Look for the library.
find_library(TBB_MALLOC_PROXY_LIBRARY NAMES tbbmalloc_proxy)

# Handle the QUIETLY and REQUIRED arguments and set UCHARDET_FOUND to TRUE if all listed variables are TRUE.
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  tbbmalloc_proxy
  DEFAULT_MSG
  TBB_MALLOC_PROXY_LIBRARY
  TBB_MALLOC_PROXY_INCLUDE_DIR
)

# Copy the results to the output variables.
if(tbbmalloc_proxy_FOUND)
  set(TBB_MALLOC_PROXY_LIBRARIES ${TBB_MALLOC_PROXY_LIBRARY})
  set(TBB_MALLOC_PROXY_INCLUDE_DIRS ${TBB_MALLOC_PROXY_INCLUDE_DIR})
else(tbbmalloc_proxy_FOUND)
  set(TBB_MALLOC_PROXY_LIBRARIES)
  set(TBB_MALLOC_PROXY_INCLUDE_DIRS)
endif(tbbmalloc_proxy_FOUND)

mark_as_advanced(TBB_MALLOC_PROXY_LIBRARIES TBB_MALLOC_PROXY_INCLUDE_DIRS)
