# - Find xxHash
# Find the native xxHash headers and libraries.
#
# XXHASH_INCLUDE_DIRS	- where to find xxhash.h, etc.
# XXHASH_LIBRARIES	- List of libraries when using xxHash.
# XXHASH_FOUND	- True if xxHash found.

# Look for the header file.
find_path(
  XXHASH_INCLUDE_DIR
  NAMES xxhash.h
  PATH_SUFFIXES xxhash
)

# Look for the library.
find_library(XXHASH_LIBRARY NAMES xxhash)

# Handle the QUIETLY and REQUIRED arguments and set UCHARDET_FOUND to TRUE if all listed variables are TRUE.
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  xxHash
  DEFAULT_MSG
  XXHASH_LIBRARY
  XXHASH_INCLUDE_DIR
)

# Copy the results to the output variables.
if(XXHASH_FOUND)
  set(XXHASH_LIBRARIES ${XXHASH_LIBRARY})
  set(XXHASH_INCLUDE_DIRS ${XXHASH_INCLUDE_DIR})
else(XXHASH_FOUND)
  set(XXHASH_LIBRARIES)
  set(XXHASH_INCLUDE_DIRS)
endif(XXHASH_FOUND)

mark_as_advanced(XXHASH_INCLUDE_DIRS XXHASH_LIBRARIES)
