# - Find robin-hood-hashing
# Find the native robin-hood-hashing headers.
#
# ROBIN_HOOD_INCLUDE_DIRS	- where to find robin_hood.h, etc.
# ROBIN_HOOD_FOUND	- True if robin_hood found.

# Look for the header file.
find_path(ROBIN_HOOD_INCLUDE_DIR NAMES robin_hood.h)

# Handle the QUIETLY and REQUIRED arguments and set UCHARDET_FOUND to TRUE if all listed variables are TRUE.
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(robin_hood DEFAULT_MSG ROBIN_HOOD_INCLUDE_DIR)

# Copy the results to the output variables.
set(ROBIN_HOOD_LIBRARIES)
if(ROBIN_HOOD_FOUND)
  set(ROBIN_HOOD_INCLUDE_DIRS ${ROBIN_HOOD_INCLUDE_DIR})
else(ROBIN_HOOD_FOUND)
  set(ROBIN_HOOD_INCLUDE_DIRS)
endif(ROBIN_HOOD_FOUND)

mark_as_advanced(ROBIN_HOOD_INCLUDE_DIRS)
