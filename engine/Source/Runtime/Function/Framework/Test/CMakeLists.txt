# =====================================================================
# Unity-parity smoke test.
#
#   cmake -B build -DZENGINE_BUILD_UNITY_PARITY_SMOKE_TEST=ON
#   cmake --build build --target UnityParitySmokeTest
#   ctest --test-dir build -C Debug -R UnityParitySmokeTest
#
# Own main(); carved out of the ZRuntime GLOB_RECURSE in Runtime/CMakeLists.txt
# (both SOURCE_FILES and REGULAR_SOURCE_FILES) so /WHOLEARCHIVE cannot drag it
# into every consumer executable (LNK2005).
#
# Headless: no project, no window, no GPU, no audio device. Runs everywhere.
# =====================================================================

option(ZENGINE_BUILD_UNITY_PARITY_SMOKE_TEST
       "Build the Unity API parity smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_UNITY_PARITY_SMOKE_TEST)
    return()
endif()

add_executable(UnityParitySmokeTest UnityParitySmokeTest.cpp)

set_target_properties(UnityParitySmokeTest PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

# Needs ZRuntime: TagLayerManager is compiled into it and GameObject lives there.
target_link_libraries(UnityParitySmokeTest PRIVATE ZRuntime)
target_include_directories(UnityParitySmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source)

if(TARGET ZEnginePCH)
    target_link_libraries(UnityParitySmokeTest PRIVATE ZEnginePCH)
    target_include_directories(UnityParitySmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(UnityParitySmokeTest ZEnginePCH)
    target_precompile_headers(UnityParitySmokeTest REUSE_FROM ZEnginePCH)
endif()

if(MSVC)
    target_compile_options(UnityParitySmokeTest PRIVATE /W4)
endif()

if(APPLE)
    target_sources(UnityParitySmokeTest PRIVATE
        ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
endif()

# The wiring gate reads engine sources off disk; it must look at the tree it was
# built from, not at a hard-coded drive letter.
target_compile_definitions(UnityParitySmokeTest PRIVATE
    "ZENGINE_ENGINE_ROOT_DIR=\"${ENGINE_ROOT_DIR}\"")

add_test(NAME UnityParitySmokeTest COMMAND UnityParitySmokeTest)
message(STATUS "Configured Unity parity smoke-test target: UnityParitySmokeTest")
