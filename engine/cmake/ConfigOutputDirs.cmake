# ConfigOutputDirs.cmake
# Sets per-config output directories (bin/Debug, bin/Release, etc.) for executables
# and shared libraries so that Debug/Release builds do not overwrite each other.
#
# Usage:
#   z_set_config_output_dirs(<target> <output_base_dir> [RUNTIME|LIBRARY] [subdir])
#
# Arguments:
#   target          - CMake target name (executable or shared library)
#   output_base_dir - Base directory (e.g. ${BINARY_ROOT_DIR}); config subdirs will be created under it
#   RUNTIME         - For executables (default): sets RUNTIME_OUTPUT_DIRECTORY_* properties
#   LIBRARY         - For shared libraries (DLL/.so): sets LIBRARY_OUTPUT_DIRECTORY_* and
#                     ARCHIVE_OUTPUT_DIRECTORY_* (import lib on Windows) so Debug/Release do not mix
#   subdir          - Optional extra level *below* the config directory, for a
#                     tool that ships more than one file and would otherwise
#                     scatter them across a shared directory:
#                         <output_base_dir>/<Config>/<subdir>
#                     Omit it and the layout is the plain <output_base_dir>/<Config>.
#
function(z_set_config_output_dirs TARGET OUTPUT_BASE_DIR)
  if(ARGC LESS 2)
    message(FATAL_ERROR "z_set_config_output_dirs requires at least: target and output_base_dir")
  endif()
  set(OUTPUT_TYPE "RUNTIME")
  if(ARGC GREATER 2)
    if(ARGV2 STREQUAL "LIBRARY")
      set(OUTPUT_TYPE "LIBRARY")
    elseif(ARGV2 STREQUAL "RUNTIME")
      set(OUTPUT_TYPE "RUNTIME")
    else()
      message(FATAL_ERROR "z_set_config_output_dirs third argument must be RUNTIME or LIBRARY, got: ${ARGV2}")
    endif()
  endif()
  # Empty when omitted, so every path below reads exactly as it did before.
  set(OUTPUT_SUBDIR "")
  if(ARGC GREATER 3 AND ARGV3)
    set(OUTPUT_SUBDIR "/${ARGV3}")
  endif()
  # For LIBRARY type (shared libraries): On Windows, DLLs go to RUNTIME_OUTPUT_DIRECTORY,
  # not LIBRARY_OUTPUT_DIRECTORY. LIBRARY_OUTPUT_DIRECTORY is for Unix .so files.
  # We need to set RUNTIME for the DLL itself and ARCHIVE for the import library.
  if(OUTPUT_TYPE STREQUAL "LIBRARY")
    if(WIN32)
      # Windows: DLL goes to RUNTIME, import lib (.lib) goes to ARCHIVE
      set_target_properties(${TARGET} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${OUTPUT_BASE_DIR}/Debug${OUTPUT_SUBDIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUTPUT_BASE_DIR}/Release${OUTPUT_SUBDIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OUTPUT_BASE_DIR}/RelWithDebInfo${OUTPUT_SUBDIR}"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${OUTPUT_BASE_DIR}/MinSizeRel${OUTPUT_SUBDIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUGV8 "${OUTPUT_BASE_DIR}/DebugV8${OUTPUT_SUBDIR}"
        ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${OUTPUT_BASE_DIR}/Debug${OUTPUT_SUBDIR}"
        ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${OUTPUT_BASE_DIR}/Release${OUTPUT_SUBDIR}"
        ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OUTPUT_BASE_DIR}/RelWithDebInfo${OUTPUT_SUBDIR}"
        ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL "${OUTPUT_BASE_DIR}/MinSizeRel${OUTPUT_SUBDIR}"
        ARCHIVE_OUTPUT_DIRECTORY_DEBUGV8 "${OUTPUT_BASE_DIR}/DebugV8${OUTPUT_SUBDIR}"
      )
    else()
      # Unix: .so goes to LIBRARY
      set_target_properties(${TARGET} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${OUTPUT_BASE_DIR}/Debug${OUTPUT_SUBDIR}"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${OUTPUT_BASE_DIR}/Release${OUTPUT_SUBDIR}"
        LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OUTPUT_BASE_DIR}/RelWithDebInfo${OUTPUT_SUBDIR}"
        LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL "${OUTPUT_BASE_DIR}/MinSizeRel${OUTPUT_SUBDIR}"
      )
    endif()
  else()
    # RUNTIME type (executables)
    set_target_properties(${TARGET} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY_DEBUG "${OUTPUT_BASE_DIR}/Debug${OUTPUT_SUBDIR}"
      RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUTPUT_BASE_DIR}/Release${OUTPUT_SUBDIR}"
      RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OUTPUT_BASE_DIR}/RelWithDebInfo${OUTPUT_SUBDIR}"
      RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${OUTPUT_BASE_DIR}/MinSizeRel${OUTPUT_SUBDIR}"
      RUNTIME_OUTPUT_DIRECTORY_DEBUGV8 "${OUTPUT_BASE_DIR}/DebugV8${OUTPUT_SUBDIR}"
    )
  endif()
  message(STATUS "${TARGET} ${OUTPUT_TYPE} output directory: ${OUTPUT_BASE_DIR}/<Config>${OUTPUT_SUBDIR} (Debug, Release, etc.)")
endfunction()

# z_set_config_archive_dirs
# Sets per-config ARCHIVE output directories for a static (or import) library target.
# Use for multi-config generators (Visual Studio, Ninja Multi-Config) so Debug/Release
# .lib files do not overwrite each other (avoids _ITERATOR_DEBUG_LEVEL mismatch).
#
# Usage:
#   z_set_config_archive_dirs(<target> <output_base_dir> [subdir])
#
# subdir behaves exactly as in z_set_config_output_dirs(): an extra level below
# the config directory.
#
function(z_set_config_archive_dirs TARGET OUTPUT_BASE_DIR)
  if(NOT CMAKE_GENERATOR MATCHES "Visual Studio|Ninja Multi-Config")
    return()
  endif()
  set(OUTPUT_SUBDIR "")
  if(ARGC GREATER 2 AND ARGV2)
    set(OUTPUT_SUBDIR "/${ARGV2}")
  endif()
  set_target_properties(${TARGET} PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${OUTPUT_BASE_DIR}/Debug${OUTPUT_SUBDIR}"
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${OUTPUT_BASE_DIR}/Release${OUTPUT_SUBDIR}"
    ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OUTPUT_BASE_DIR}/RelWithDebInfo${OUTPUT_SUBDIR}"
    ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL "${OUTPUT_BASE_DIR}/MinSizeRel${OUTPUT_SUBDIR}"
    ARCHIVE_OUTPUT_DIRECTORY_DEBUGV8 "${OUTPUT_BASE_DIR}/DebugV8${OUTPUT_SUBDIR}"
  )
  message(STATUS "${TARGET} ARCHIVE output directory: ${OUTPUT_BASE_DIR}/<Config>${OUTPUT_SUBDIR}")
endfunction()
