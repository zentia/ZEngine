# ConfigOutputDirs.cmake
# Sets per-config output directories (bin/Debug, bin/Release, etc.) for executables
# and shared libraries so that Debug/Release builds do not overwrite each other.
#
# Usage:
#   z_set_config_output_dirs(<target> <output_base_dir> [RUNTIME|LIBRARY])
#
# Arguments:
#   target          - CMake target name (executable or shared library)
#   output_base_dir - Base directory (e.g. ${BINARY_ROOT_DIR}); config subdirs will be created under it
#   RUNTIME         - For executables (default): sets RUNTIME_OUTPUT_DIRECTORY_* properties
#   LIBRARY         - For shared libraries (DLL/.so): sets LIBRARY_OUTPUT_DIRECTORY_* and
#                     ARCHIVE_OUTPUT_DIRECTORY_* (import lib on Windows) so Debug/Release do not mix
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
  # For LIBRARY type (shared libraries): On Windows, DLLs go to RUNTIME_OUTPUT_DIRECTORY,
  # not LIBRARY_OUTPUT_DIRECTORY. LIBRARY_OUTPUT_DIRECTORY is for Unix .so files.
  # We need to set RUNTIME for the DLL itself and ARCHIVE for the import library.
  if(OUTPUT_TYPE STREQUAL "LIBRARY")
    if(WIN32)
      # Windows: DLL goes to RUNTIME, import lib (.lib) goes to ARCHIVE
      set_target_properties(${TARGET} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${OUTPUT_BASE_DIR}/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUTPUT_BASE_DIR}/Release"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OUTPUT_BASE_DIR}/RelWithDebInfo"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${OUTPUT_BASE_DIR}/MinSizeRel"
        RUNTIME_OUTPUT_DIRECTORY_DEBUGV8 "${OUTPUT_BASE_DIR}/DebugV8"
        ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${OUTPUT_BASE_DIR}/Debug"
        ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${OUTPUT_BASE_DIR}/Release"
        ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OUTPUT_BASE_DIR}/RelWithDebInfo"
        ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL "${OUTPUT_BASE_DIR}/MinSizeRel"
        ARCHIVE_OUTPUT_DIRECTORY_DEBUGV8 "${OUTPUT_BASE_DIR}/DebugV8"
      )
    else()
      # Unix: .so goes to LIBRARY
      set_target_properties(${TARGET} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${OUTPUT_BASE_DIR}/Debug"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${OUTPUT_BASE_DIR}/Release"
        LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OUTPUT_BASE_DIR}/RelWithDebInfo"
        LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL "${OUTPUT_BASE_DIR}/MinSizeRel"
      )
    endif()
  else()
    # RUNTIME type (executables)
    set_target_properties(${TARGET} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY_DEBUG "${OUTPUT_BASE_DIR}/Debug"
      RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUTPUT_BASE_DIR}/Release"
      RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OUTPUT_BASE_DIR}/RelWithDebInfo"
      RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${OUTPUT_BASE_DIR}/MinSizeRel"
      RUNTIME_OUTPUT_DIRECTORY_DEBUGV8 "${OUTPUT_BASE_DIR}/DebugV8"
    )
  endif()
  message(STATUS "${TARGET} ${OUTPUT_TYPE} output directory: ${OUTPUT_BASE_DIR}/<Config> (Debug, Release, etc.)")
endfunction()

# z_set_config_archive_dirs
# Sets per-config ARCHIVE output directories for a static (or import) library target.
# Use for multi-config generators (Visual Studio, Ninja Multi-Config) so Debug/Release
# .lib files do not overwrite each other (avoids _ITERATOR_DEBUG_LEVEL mismatch).
#
# Usage:
#   z_set_config_archive_dirs(<target> <output_base_dir>)
#
function(z_set_config_archive_dirs TARGET OUTPUT_BASE_DIR)
  if(NOT CMAKE_GENERATOR MATCHES "Visual Studio|Ninja Multi-Config")
    return()
  endif()
  set_target_properties(${TARGET} PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${OUTPUT_BASE_DIR}/Debug"
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${OUTPUT_BASE_DIR}/Release"
    ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OUTPUT_BASE_DIR}/RelWithDebInfo"
    ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL "${OUTPUT_BASE_DIR}/MinSizeRel"
    ARCHIVE_OUTPUT_DIRECTORY_DEBUGV8 "${OUTPUT_BASE_DIR}/DebugV8"
  )
  message(STATUS "${TARGET} ARCHIVE output directory: ${OUTPUT_BASE_DIR}/<Config>")
endfunction()
