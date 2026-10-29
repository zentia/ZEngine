# ============================================================================
# ZSlate Examples CMakeLists.txt
# ============================================================================
# This CMakeLists.txt shows how to build standalone applications
# that use the ZSlate UI framework.
# ============================================================================

cmake_minimum_required(VERSION 3.20)
project(ZSlateExamples LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find ZSlate library
# Option 1: Use find_package when ZSlate is installed
# find_package(ZSlate REQUIRED)

# Option 2: Build from source (for development)
# The ZSlate library should be added as a subdirectory
# from the parent CMakeLists.txt, or you can include it directly:
# add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. ${CMAKE_CURRENT_BINARY_DIR}/ZSlate)

# For this example, we assume ZSlate is available as a target
if(NOT TARGET ZSlate)
    message(WARNING "ZSlate target not found. Examples will not be built.")
    return()
endif()

# ============================================================================
# Example 1: Simple Window (console output)
# ============================================================================

add_executable(ZSlateSimpleWindow simple_window.cpp)
target_link_libraries(ZSlateSimpleWindow PRIVATE ZSlate)

# ============================================================================
# Example 2: Standalone Application (full demo)
# ============================================================================

add_executable(ZSlateStandaloneApp standalone_app_example.cpp)
target_link_libraries(ZSlateStandaloneApp PRIVATE ZSlate)

# ============================================================================
# Common Settings
# ============================================================================

# Compile options
if(MSVC)
    target_compile_options(ZSlateSimpleWindow PRIVATE /W4 /permissive-)
    target_compile_options(ZSlateStandaloneApp PRIVATE /W4 /permissive-)
    target_compile_definitions(ZSlateSimpleWindow PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_definitions(ZSlateStandaloneApp PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ZSlateSimpleWindow PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ZSlateStandaloneApp PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Output directories
set_target_properties(ZSlateSimpleWindow PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
set_target_properties(ZSlateStandaloneApp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

message(STATUS "ZSlate Examples: building ZSlateSimpleWindow, ZSlateStandaloneApp")
