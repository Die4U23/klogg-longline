# CMake 3.29 can decode cl's UTF-8 /showIncludes output using the Windows
# ANSI code page when launched without a console. Ninja then records zero
# header dependencies. Preserve the compiler's bytes instead of transcoding.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
  set(_probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles/KloggShowIncludes")
  file(MAKE_DIRECTORY "${_probe_dir}")
  file(WRITE "${_probe_dir}/foo.h" "\n")
  file(WRITE "${_probe_dir}/main.c" "#include \"foo.h\"\nint main(void) { return 0; }\n")
  execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" /nologo /showIncludes /c main.c
    WORKING_DIRECTORY "${_probe_dir}"
    OUTPUT_FILE "${_probe_dir}/stdout.bin"
    ERROR_FILE "${_probe_dir}/stderr.bin"
    RESULT_VARIABLE _probe_result)
  file(READ "${_probe_dir}/stdout.bin" _probe_output)
  # Same prefix grammar as CMakeDetermineCompilerId.cmake, applied to raw bytes.
  if(_probe_result EQUAL 0 AND "${_probe_output}" MATCHES "(^|\n)([^:\n][^:\n]+:[^:\n]*[^: \n][^: \n]:?[ \t]+)([A-Za-z]:\\\\|\\./|\\.\\\\|/)")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}")
    set(CMAKE_C_CL_SHOWINCLUDES_PREFIX "${CMAKE_CL_SHOWINCLUDES_PREFIX}")
    set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "${CMAKE_CL_SHOWINCLUDES_PREFIX}")
    message(STATUS "MSVC header dependency prefix detected without transcoding")
  else()
    message(FATAL_ERROR "Cannot detect MSVC header dependency prefix; inspect ${_probe_dir}")
  endif()
  unset(_probe_dir)
  unset(_probe_output)
  unset(_probe_result)
endif()
