cmake_minimum_required(VERSION 3.24)

# ZEngine wrapper: drive the vendored renderdoc.sln with CMake-selected VS/MSBuild.
# Upstream RenderDoc disables its own CMake on WIN32; this project never includes
# upstream CMakeLists.txt and avoids opening renderdoc.sln in the IDE (no solution
# upgrade / toolset redirect prompts).
#
# Lives outside the renderdoc submodule (tools/renderdoc_build/cmake).

project(ZRenderDocBuild NONE)

set(RENDERDOC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../renderdoc" CACHE PATH "Vendored RenderDoc submodule root (contains renderdoc.sln)")

if(NOT EXISTS "${RENDERDOC_ROOT}/renderdoc.sln")
    message(FATAL_ERROR "renderdoc.sln not found under ${RENDERDOC_ROOT}")
endif()

set(RENDERDOC_CONFIG "Development" CACHE STRING "MSBuild configuration (Development or Release)")
set_property(CACHE RENDERDOC_CONFIG PROPERTY STRINGS Development Release)

set(RENDERDOC_PLATFORM "x64" CACHE STRING "MSBuild platform")
set(RENDERDOC_PLATFORM_TOOLSET "v143" CACHE STRING "MSBuild /p:PlatformToolset=... (v140 in sln, override to v143 for VS 2022)")
set(RENDERDOC_VCTOOLS_VERSION "" CACHE STRING "Optional /p:VCToolsVersion=... (e.g. 14.44.35207)")

if(RENDERDOC_VCTOOLS_VERSION STREQUAL "")
    set(_z_msvc_roots
        "C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/MSVC"
        "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Tools/MSVC"
        "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC"
        "C:/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC"
    )
    foreach(_z_root IN LISTS _z_msvc_roots)
        if(EXISTS "${_z_root}/14.44.35207")
            set(RENDERDOC_VCTOOLS_VERSION "14.44.35207" CACHE STRING "" FORCE)
            break()
        endif()
    endforeach()
endif()

if(CMAKE_VS_MSBUILD_COMMAND)
    set(_renderdoc_msbuild "${CMAKE_VS_MSBUILD_COMMAND}")
else()
    find_program(_renderdoc_msbuild
        NAMES msbuild msbuild.exe
        HINTS
            "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Professional/MSBuild/Current/Bin"
            "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Enterprise/MSBuild/Current/Bin"
            "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin"
            "$ENV{ProgramFiles(x86)}/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin"
    )
endif()

if(NOT _renderdoc_msbuild)
    message(FATAL_ERROR "MSBuild not found. Configure with -G \"Visual Studio 17 2022\" -A x64, or install VS 2022 C++ workload.")
endif()

set(_renderdoc_msbuild_args
    "${RENDERDOC_ROOT}/renderdoc.sln"
    /m
    /nologo
    /v:m
    /t:Build
    "/p:Configuration=${RENDERDOC_CONFIG}"
    "/p:Platform=${RENDERDOC_PLATFORM}"
    "/p:PlatformToolset=${RENDERDOC_PLATFORM_TOOLSET}"
)

if(NOT RENDERDOC_VCTOOLS_VERSION STREQUAL "")
    list(APPEND _renderdoc_msbuild_args "/p:VCToolsVersion=${RENDERDOC_VCTOOLS_VERSION}")
endif()

if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    list(APPEND _renderdoc_msbuild_args "/p:WindowsTargetPlatformVersion=${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
endif()

add_custom_target(renderdoc ALL
    COMMAND "${_renderdoc_msbuild}" ${_renderdoc_msbuild_args}
    WORKING_DIRECTORY "${RENDERDOC_ROOT}"
    USES_TERMINAL
    COMMENT "Building RenderDoc (${RENDERDOC_CONFIG}|${RENDERDOC_PLATFORM}) via MSBuild"
)

message(STATUS "RenderDoc root: ${RENDERDOC_ROOT}")
message(STATUS "MSBuild: ${_renderdoc_msbuild}")
message(STATUS "Configuration: ${RENDERDOC_CONFIG}")
message(STATUS "Platform: ${RENDERDOC_PLATFORM}")
message(STATUS "PlatformToolset: ${RENDERDOC_PLATFORM_TOOLSET}")
if(NOT RENDERDOC_VCTOOLS_VERSION STREQUAL "")
    message(STATUS "VCToolsVersion: ${RENDERDOC_VCTOOLS_VERSION}")
endif()
if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    message(STATUS "WindowsTargetPlatformVersion: ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
endif()
