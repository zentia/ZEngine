cmake_minimum_required(VERSION 3.19 FATAL_ERROR)

# ==============================================================================
# DataTableCook -- build-time CSV -> .zasset compiler (see DataTableCook.cpp).
#
# Full-tree configure only. Unlike the other tools in engine/tools/ this one
# cannot configure standalone: it links ZRuntimeShared, and pulling
# engine/Source/Runtime into a standalone configure would drag in the entire
# engine dependency tree (DX12 / Vulkan / Jolt / Puerts / ...). The tools that
# are meant to be configurable on their own guard with
# `if(NOT TARGET <dep>) add_subdirectory(...)`; here there is nothing cheap to
# add, so we simply skip and say so.
# ==============================================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    message(STATUS "DataTableCook requires a full-tree configure (it links ZRuntimeShared); target skipped")
    return()
endif()

if(NOT WIN32)
    message(STATUS "DataTableCook is Windows-only; target skipped")
    return()
endif()

if(NOT TARGET ZRuntimeShared)
    message(STATUS "DataTableCook: ZRuntimeShared target not present; target skipped")
    return()
endif()

set(DATATABLE_COOK_TARGET DataTableCook)

add_executable(${DATATABLE_COOK_TARGET} DataTableCook.cpp)

set_target_properties(${DATATABLE_COOK_TARGET} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "DataTableCook"
    FOLDER "Tools"
)

target_compile_definitions(${DATATABLE_COOK_TARGET} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

# ZRuntimeShared's own include dirs are PRIVATE (the engine links them per
# target), so spell out the one root the C ABI header is reached through.
# Deliberately the ONLY include dir: DataTableCook.cpp includes just
# <Runtime/ZEngineApi/ZEngineToolApi.h> and <cstdio>, which is what keeps it
# buildable without the engine PCH / EASTL on its include path.
target_include_directories(${DATATABLE_COOK_TARGET} PRIVATE "${ENGINE_ROOT_DIR}/Source")

target_link_libraries(${DATATABLE_COOK_TARGET} PRIVATE ZRuntimeShared)

if(MSVC)
    target_compile_options(${DATATABLE_COOK_TARGET} PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
    target_compile_options(${DATATABLE_COOK_TARGET} PRIVATE -Wall -Wextra -Wpedantic)
endif()

# It needs ZRuntimeShared.dll beside it. $<TARGET_FILE:...> is the DLL, not the
# import lib, for a SHARED target on Windows -- which is exactly what we want to
# place next to the .exe.
add_custom_command(TARGET ${DATATABLE_COOK_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:ZRuntimeShared>"
        "$<TARGET_FILE_DIR:${DATATABLE_COOK_TARGET}>"
)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/ToolOutputDir.cmake")
z_set_tool_output_dir(${DATATABLE_COOK_TARGET} SUBDIR "DataTableCook")
