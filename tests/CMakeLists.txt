# =============================================================================
# ZEngine Platform Tests
# =============================================================================
# This file contains all platform detection tests for ZEngine

cmake_minimum_required(VERSION 3.19)

# =============================================================================
# Platform Test Executables
# =============================================================================

# Simple platform test executable
add_executable(PlatformTestSimple test_simple.cpp)
set_target_properties(PlatformTestSimple PROPERTIES
    FOLDER "Tests/Platform"
    OUTPUT_NAME "platform_test_simple"
)

# Basic platform test executable
add_executable(PlatformTest platform_example.cpp)
set_target_properties(PlatformTest PROPERTIES
    FOLDER "Tests/Platform"
    OUTPUT_NAME "platform_test"
)

# Advanced platform test executable
add_executable(PlatformTestAdvanced test_advanced.cpp)
set_target_properties(PlatformTestAdvanced PROPERTIES
    FOLDER "Tests/Platform"
    OUTPUT_NAME "platform_test_advanced"
)

# Add include directories
target_include_directories(PlatformTestSimple PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(PlatformTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(PlatformTestAdvanced PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Platform-specific settings
if(WIN32)
    set_target_properties(PlatformTestSimple PROPERTIES WIN32_EXECUTABLE FALSE)
    set_target_properties(PlatformTest PROPERTIES WIN32_EXECUTABLE FALSE)
    set_target_properties(PlatformTestAdvanced PROPERTIES WIN32_EXECUTABLE FALSE)
endif()

# =============================================================================
# Test Configuration
# =============================================================================

# Enable testing
enable_testing()

# Add tests
add_test(NAME PlatformDetectionTest 
         COMMAND PlatformTest
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME PlatformAdvancedTest 
         COMMAND PlatformTestAdvanced
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME PlatformSimpleTest 
         COMMAND PlatformTestSimple
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# Set test properties
set_tests_properties(PlatformDetectionTest PROPERTIES
    TIMEOUT 30
    PASS_REGULAR_EXPRESSION "Platform: .*"
)

set_tests_properties(PlatformAdvancedTest PROPERTIES
    TIMEOUT 60
    PASS_REGULAR_EXPRESSION "Total tests:"
)

set_tests_properties(PlatformSimpleTest PROPERTIES
    TIMEOUT 30
    PASS_REGULAR_EXPRESSION "Test Complete"
)
