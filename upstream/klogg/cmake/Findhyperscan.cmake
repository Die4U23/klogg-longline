# - Find hyperscan
# Find the native hyperscan headers and libraries.
#
# HYPERSCAN_INCLUDE_DIRS	- where to find hyperscan.h, etc.
# HYPERSCAN_LIBRARIES	- List of libraries when using hyperscan.
# HYPERSCAN_FOUND	- True if hyperscan found.

# Look for the header file.
find_path(
  HYPERSCAN_INCLUDE_DIR
  NAMES hs.h
  PATH_SUFFIXES hs
)

# Look for the library.
find_library(HYPERSCAN_LIBRARY NAMES hs)

# Handle the QUIETLY and REQUIRED arguments and set UCHARDET_FOUND to TRUE if all listed variables are TRUE.
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  hyperscan
  DEFAULT_MSG
  HYPERSCAN_LIBRARY
  HYPERSCAN_INCLUDE_DIR
)

# Copy the results to the output variables.
if(HYPERSCAN_FOUND)
  set(HYPERSCAN_LIBRARIES ${HYPERSCAN_LIBRARY})
  set(HYPERSCAN_INCLUDE_DIRS ${HYPERSCAN_INCLUDE_DIR})
else(HYPERSCAN_FOUND)
  set(HYPERSCAN_LIBRARIES)
  set(HYPERSCAN_INCLUDE_DIRS)
endif(HYPERSCAN_FOUND)

mark_as_advanced(HYPERSCAN_INCLUDE_DIRS HYPERSCAN_LIBRARIES)
