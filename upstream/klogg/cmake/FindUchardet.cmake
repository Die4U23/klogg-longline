# - Find uchardet
# Find the native uchardet headers and libraries.
#
# UCHARDET_INCLUDE_DIRS	- where to find UCHARDET.h, etc.
# UCHARDET_LIBRARIES	- List of libraries when using UCHARDET.
# UCHARDET_FOUND	- True if UCHARDET found.

# Look for the header file.
find_path(
  UCHARDET_INCLUDE_DIR
  NAMES uchardet.h
  PATH_SUFFIXES uchardet
)

# Look for the library.
find_library(UCHARDET_LIBRARY NAMES uchardet)

# Handle the QUIETLY and REQUIRED arguments and set UCHARDET_FOUND to TRUE if all listed variables are TRUE.
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  Uchardet
  DEFAULT_MSG
  UCHARDET_LIBRARY
  UCHARDET_INCLUDE_DIR
)

# Copy the results to the output variables.
if(UCHARDET_FOUND)
  set(UCHARDET_LIBRARIES ${UCHARDET_LIBRARY})
  set(UCHARDET_INCLUDE_DIRS ${UCHARDET_INCLUDE_DIR})
else(UCHARDET_FOUND)
  set(UCHARDET_LIBRARIES)
  set(UCHARDET_INCLUDE_DIRS)
endif(UCHARDET_FOUND)

mark_as_advanced(UCHARDET_INCLUDE_DIRS UCHARDET_LIBRARIES)
