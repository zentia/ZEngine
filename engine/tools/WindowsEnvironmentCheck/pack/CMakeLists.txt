cmake_minimum_required(VERSION 3.19 FATAL_ERROR)

# Packaging for ZEnvironmentCollector, modelled on BianqueCCS (see its
# pack/CMakeLists.txt): a project that compiles nothing and only turns a staged
# install tree into one archive with a checksum next to it.
#
# It is a separate project rather than a few lines in the tool's own
# CMakeLists.txt because that file is also add_subdirectory()d by a full engine
# tree -- CPack would then configure a package nobody asked for, once per
# configure, in a build that was never meant to ship anything.
#
# Usage (from the repository root):
#   cmake --build build\tools\ZEnvironmentCollector --config Release --target ZEnvironmentCollector
#   cmake --install build\tools\ZEnvironmentCollector --config Release ^
#         --component EnvironmentCollector --prefix dist\staging
#   cmake -S engine\tools\WindowsEnvironmentCheck\pack -B build\tools\ZEnvironmentCollector\pack
#   cmake --build build\tools\ZEnvironmentCollector\pack --target package
project(ZEnvironmentCollectorPack LANGUAGES NONE)

get_filename_component(PACK_TOOL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
get_filename_component(PACK_REPO_ROOT "${PACK_TOOL_DIR}/../../.." ABSOLUTE)

# ---------------------------------------------------------------------------
# Version: parsed out of ToolVersion.h, the same way BianqueCCS parses
# version.cpp. One source of truth -- the archive name, the report's
# "toolVersion" field and what the tool prints cannot disagree, because there is
# only one place the number is written.
# ---------------------------------------------------------------------------
set(PACK_VERSION "")
if(DEFINED PACK_VERSION_OVERRIDE AND NOT PACK_VERSION_OVERRIDE STREQUAL "")
    set(PACK_VERSION "${PACK_VERSION_OVERRIDE}")
else()
    set(_version_header "${PACK_TOOL_DIR}/ToolVersion.h")
    if(NOT EXISTS "${_version_header}")
        message(FATAL_ERROR "ToolVersion.h not found at ${_version_header}")
    endif()
    file(READ "${_version_header}" _version_header_text)
    string(REGEX MATCH "ToolVersion[^\"']*\"([0-9]+)\\.([0-9]+)\\.([0-9]+)\"" _version_match "${_version_header_text}")
    if(CMAKE_MATCH_1 STREQUAL "")
        message(FATAL_ERROR "Could not parse a x.y.z version out of ${_version_header}")
    endif()
    set(PACK_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
endif()

if(NOT DEFINED PACKAGE_NAME OR PACKAGE_NAME STREQUAL "")
    set(PACKAGE_NAME "zenvironment-collector")
endif()

# ---------------------------------------------------------------------------
# Architecture: same shape as BianqueCCS' BQ_DETECT_ARCH -- an environment
# override first, then whatever the generator already knows. It ends up in the
# file name, and "which build is this" is the first question anyone asks about a
# downloaded archive.
# ---------------------------------------------------------------------------
set(DETECTED_ARCH "")
if(DEFINED ENV{USER_DEF_ARCH} AND NOT "$ENV{USER_DEF_ARCH}" STREQUAL "")
    set(DETECTED_ARCH "$ENV{USER_DEF_ARCH}")
else()
    # What the generator was asked for first: it is the only hint that describes
    # the bits actually built. CMAKE_SIZEOF_VOID_P is the usual tie-breaker
    # elsewhere but it is empty in this project -- LANGUAGES NONE means no
    # compiler was ever asked -- so it can only be a last resort.
    set(_arch_hint "${CMAKE_GENERATOR_PLATFORM}")
    if(_arch_hint STREQUAL "")
        set(_arch_hint "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    if(_arch_hint MATCHES "ARM64|arm64|aarch64|AARCH64")
        set(DETECTED_ARCH "arm64")
    elseif(_arch_hint MATCHES "x64|AMD64|amd64|x86_64|X86_64|EM64T")
        set(DETECTED_ARCH "x86_64")
    elseif(_arch_hint MATCHES "Win32|x86|i386|i686")
        set(DETECTED_ARCH "x86")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(DETECTED_ARCH "x86_64")
    else()
        set(DETECTED_ARCH "x86_64")
        message(WARNING "Could not tell the target architecture (hint was '${_arch_hint}'); "
                        "defaulting to x86_64. Set USER_DEF_ARCH to override.")
    endif()
    message(STATUS "Detected architecture: ${DETECTED_ARCH} (hint: '${_arch_hint}')")
endif()

# ---------------------------------------------------------------------------
# What goes in: the install tree, already staged by `cmake --install`. The
# staging directory is the package root, so the archive opens as
# WindowsEnvironmentCheck/ (exe + ZDllLoadProbe.exe + profiles/ + LICENSEs) plus
# README.md -- exactly the folder that runs, with no extra nesting to unwrap.
# ---------------------------------------------------------------------------
if(NOT DEFINED PACK_STAGING_DIR OR PACK_STAGING_DIR STREQUAL "")
    set(PACK_STAGING_DIR "${PACK_REPO_ROOT}/dist/staging")
endif()
if(NOT DEFINED PACK_OUTPUT_DIR OR PACK_OUTPUT_DIR STREQUAL "")
    set(PACK_OUTPUT_DIR "${PACK_REPO_ROOT}/dist")
endif()

if(NOT EXISTS "${PACK_STAGING_DIR}/WindowsEnvironmentCheck/ZEnvironmentCollector.exe")
    message(FATAL_ERROR
        "No staged build found at ${PACK_STAGING_DIR}. Run this first (from the "
        "repository root):\n"
        "  cmake --install build\\tools\\ZEnvironmentCollector --config Release "
        "--component EnvironmentCollector --prefix dist\\staging\n"
        "Packing a stale or empty staging directory is the one failure here that "
        "looks like success -- it produces a plausible archive that runs nothing.")
endif()

set(PACK_FILE_NAME "${PACKAGE_NAME}-windows-${DETECTED_ARCH}-${PACK_VERSION}")

set(CPACK_INSTALLED_DIRECTORIES "${PACK_STAGING_DIR};.")
set(CPACK_PACKAGE_NAME "${PACKAGE_NAME}")
set(CPACK_PACKAGE_VERSION "${PACK_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "${PACK_FILE_NAME}")
set(CPACK_OUTPUT_FILE_PREFIX "${PACK_OUTPUT_DIR}")
# Sidecar checksum, like BianqueCCS: whoever downloads the archive can verify it
# without asking anyone what the hash is supposed to be.
set(CPACK_PACKAGE_CHECKSUM "SHA256")
# ZIP, not TGZ: this is a Windows-only tool and the thing on the other end is a
# launcher or a support engineer double-clicking it.
set(CPACK_GENERATOR "ZIP")
set(CPACK_VERBATIM_VARIABLES YES)
set(CPACK_SOURCE_IGNORE_FILES "")

message(STATUS "Packaging ${PACK_FILE_NAME}.zip")
message(STATUS "  from   ${PACK_STAGING_DIR}")
message(STATUS "  to     ${PACK_OUTPUT_DIR}")

include(CPack)
