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
#   cmake -S engine\tools\ZEnvironmentCollector\pack -B build\tools\ZEnvironmentCollector\pack
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
# ZEnvironmentCollector/ (exe + ZDllLoadProbe.exe + profiles/ + LICENSEs) plus
# README.md -- exactly the folder that runs, with no extra nesting to unwrap.
# ---------------------------------------------------------------------------
if(NOT DEFINED PACK_STAGING_DIR OR PACK_STAGING_DIR STREQUAL "")
    set(PACK_STAGING_DIR "${PACK_REPO_ROOT}/dist/staging")
endif()
if(NOT DEFINED PACK_OUTPUT_DIR OR PACK_OUTPUT_DIR STREQUAL "")
    set(PACK_OUTPUT_DIR "${PACK_REPO_ROOT}/dist")
endif()

# ---------------------------------------------------------------------------
# Which backend is this build?
#
# The collector has two configurations that produce different directories:
# standalone (self-contained) and engine-backed, which links ZRuntimeShared.dll
# and needs it beside the exe -- without it the process dies at startup with
# 0xc0000135 before main() runs, and the archive still looks perfectly fine.
# Both configurations would otherwise produce identically named archives, so
# "does this exe import a DLL" has to be settled here rather than left to the
# person who downloads it.
#
# The authoritative answer is the exe's own import table: it is what actually
# decides whether the process starts. Reading it means reading the file as hex
# and looking for the ASCII import name; there is no PE parser in CMake.
#
# The name is matched as an ASCIIZ string -- NUL on both sides -- because a bare
# substring match does hit other bytes: scanning this very exe for
# "ucrtbased.dll" matches, yet dumpbin /dependents shows only release runtimes.
# A false positive here is not harmless either: it would accuse a correct
# self-contained package of missing a DLL and block the pack.
# ---------------------------------------------------------------------------
function(pack_exe_imports_dll EXE_PATH DLL_NAME OUT_VAR)
    file(READ "${EXE_PATH}" _exe_hex HEX)
    string(REGEX MATCH "${DLL_NAME}" _found "${_exe_hex}")
    if(_found STREQUAL "")
        set(${OUT_VAR} FALSE PARENT_SCOPE)
    else()
        set(${OUT_VAR} TRUE PARENT_SCOPE)
    endif()
endfunction()

if(NOT EXISTS "${PACK_STAGING_DIR}/ZEnvironmentCollector/ZEnvironmentCollector.exe")
    message(FATAL_ERROR
        "No staged build found at ${PACK_STAGING_DIR}. Run this first (from the "
        "repository root):\n"
        "  cmake --install build\\tools\\ZEnvironmentCollector --config Release "
        "--component EnvironmentCollector --prefix dist\\staging\n"
        "Packing a stale or empty staging directory is the one failure here that "
        "looks like success -- it produces a plausible archive that runs nothing.")
endif()

set(PACK_RUNTIME_DIR "${PACK_STAGING_DIR}/ZEnvironmentCollector")
# "ZRuntimeShared.dll" in hex, wrapped in the NULs that make it an import name.
pack_exe_imports_dll("${PACK_RUNTIME_DIR}/ZEnvironmentCollector.exe"
    "005a52756e74696d655368617265642e646c6c00" PACK_ENGINE_BACKED)

if(PACK_ENGINE_BACKED)
    # Engine-backed: the exe links the DLL's import library, so a missing DLL is
    # a process that never starts, not a degraded feature.
    if(NOT EXISTS "${PACK_RUNTIME_DIR}/ZRuntimeShared.dll")
        message(FATAL_ERROR
            "This is an engine-backed build (ZEnvironmentCollector.exe imports "
            "ZRuntimeShared.dll) but the DLL is not in ${PACK_RUNTIME_DIR}.\n"
            "The install rule that ships it is guarded on the ZRuntimeShared target "
            "existing, so this means the staging directory was populated by a "
            "different build than the one that produced this exe -- most often an "
            "install left over from a standalone configure. Re-run the install of "
            "the engine-backed build, then reconfigure and pack again.")
    endif()
    # Degraded rather than dead: with no table the UI falls back to the English
    # literals at the call sites. Worth saying out loud because the install rule
    # for the .zasset is OPTIONAL, so its absence is otherwise silent.
    if(NOT EXISTS "${PACK_RUNTIME_DIR}/resource/ui_strings_zh-CN.zasset")
        message(WARNING
            "Engine-backed build without resource/ui_strings_zh-CN.zasset: the "
            "packaged UI will fall back to English. DataTableCook did not produce "
            "the table for this build.")
    endif()
    message(STATUS "Build backend: engine-backed (needs ZRuntimeShared.dll)")
else()
    if(EXISTS "${PACK_RUNTIME_DIR}/ZRuntimeShared.dll")
        message(WARNING
            "A ZRuntimeShared.dll sits in the staging directory but "
            "ZEnvironmentCollector.exe does not import it -- usually a stale file "
            "from an earlier engine-backed install. It will be shipped anyway; "
            "delete ${PACK_STAGING_DIR} and re-install to be sure of the contents.")
    endif()
    message(STATUS "Build backend: standalone (self-contained, no runtime DLL)")
endif()

# A staging directory gets reused, and the install rule copies README.md out of
# the source tree, so a staging populated before the README changed ships the
# old one. That is worse than shipping a stale binary: the binary fails loudly,
# a stale README does not -- and it is where the launcher contract lives
# (report file names, the four keys, the exit codes). Byte-compare it.
set(_pack_source_readme "${PACK_REPO_ROOT}/engine/tools/ZEnvironmentCollector/README.md")
if(EXISTS "${_pack_source_readme}" AND EXISTS "${PACK_STAGING_DIR}/README.md")
    file(READ "${_pack_source_readme}" _pack_readme_now)
    file(READ "${PACK_STAGING_DIR}/README.md" _pack_readme_staged)
    if(NOT _pack_readme_now STREQUAL _pack_readme_staged)
        message(WARNING
            "README.md in ${PACK_STAGING_DIR} is not the one in the source tree: "
            "this staging directory predates a README change. Re-run the install "
            "before packing, or the archive ships the old launcher contract.")
    endif()
endif()

# Two backends, and until now one identical file name for both of them -- which
# is the actual hazard: an archive that has to be opened before anyone can tell
# which build it came from, or whether it needs a runtime DLL beside it. The
# engine-backed one carries a qualifier; the standalone name is left alone
# because that is what anything already consuming it expects.
if(PACK_ENGINE_BACKED)
    set(PACK_FILE_NAME "${PACKAGE_NAME}-zruntime-windows-${DETECTED_ARCH}-${PACK_VERSION}")
else()
    set(PACK_FILE_NAME "${PACKAGE_NAME}-windows-${DETECTED_ARCH}-${PACK_VERSION}")
endif()

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
