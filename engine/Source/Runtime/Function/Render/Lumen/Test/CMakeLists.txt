# =====================================================================
# Lumen scene-geometry smoke test.
#
# Same "own main()" / side-target pattern as PlayerSettings/Test: the source is
# carved out of the ZRuntime glob in Runtime/CMakeLists.txt so its main() cannot
# be dragged into every consumer executable (LNK2005).
#
#   cmake -B build -DZENGINE_BUILD_LUMEN_SCENE_GEOMETRY_SMOKE_TEST=ON
#   cmake --build build --target LumenSceneGeometrySmokeTest
#
# Pure CPU work -- no RHI, no window, no project -- so it runs headless on every
# host that builds ZRuntime (Apple included; DX12/Windows builds strip Lumen
# entirely, where the target is simply not configured).
# =====================================================================

option(ZENGINE_BUILD_LUMEN_SCENE_GEOMETRY_SMOKE_TEST
       "Build the Lumen scene-geometry smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_LUMEN_SCENE_GEOMETRY_SMOKE_TEST)
    return()
endif()

add_executable(LumenSceneGeometrySmokeTest LumenSceneGeometrySmokeTest.cpp)

# ---------------------------------------------------------------------
# Software ray-tracing smoke test. Same pattern, separate option: it pins the
# fixed marcher (argument wins over member, voxel-scaled hit threshold, box
# clipping) and the end-to-end "GI is no longer identically zero" result.
# ---------------------------------------------------------------------
option(ZENGINE_BUILD_LUMEN_RAY_TRACING_SMOKE_TEST
       "Build the Lumen software ray-tracing smoke-test executable"
       OFF)

if(ZENGINE_BUILD_LUMEN_RAY_TRACING_SMOKE_TEST)
    add_executable(LumenRayTracingSmokeTest LumenRayTracingSmokeTest.cpp)

    set_target_properties(LumenRayTracingSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(LumenRayTracingSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(LumenRayTracingSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(LumenRayTracingSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(LumenRayTracingSmokeTest ZEnginePCH)
        target_precompile_headers(LumenRayTracingSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(LumenRayTracingSmokeTest PRIVATE /W4)
    endif()

    if(APPLE)
        target_sources(LumenRayTracingSmokeTest PRIVATE
            ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
    endif()

    add_test(NAME LumenRayTracingSmokeTest COMMAND LumenRayTracingSmokeTest)

    message(STATUS "Configured Lumen ray-tracing smoke-test target: LumenRayTracingSmokeTest")
endif()

# ---------------------------------------------------------------------
# Surface-cache smoke test. Pins that pages are actually built from the
# scene with real materials, that lookup is spatial (the old key was
# overflowing garbage), that LRU eviction works, and that a ray landing
# on a red floor reports a red albedo instead of the hard-coded grey.
# ---------------------------------------------------------------------
option(ZENGINE_BUILD_LUMEN_SURFACE_CACHE_SMOKE_TEST
       "Build the Lumen surface-cache smoke-test executable"
       OFF)

if(ZENGINE_BUILD_LUMEN_SURFACE_CACHE_SMOKE_TEST)
    add_executable(LumenSurfaceCacheSmokeTest LumenSurfaceCacheSmokeTest.cpp)

    set_target_properties(LumenSurfaceCacheSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(LumenSurfaceCacheSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(LumenSurfaceCacheSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(LumenSurfaceCacheSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(LumenSurfaceCacheSmokeTest ZEnginePCH)
        target_precompile_headers(LumenSurfaceCacheSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(LumenSurfaceCacheSmokeTest PRIVATE /W4)
    endif()

    if(APPLE)
        target_sources(LumenSurfaceCacheSmokeTest PRIVATE
            ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
    endif()

    add_test(NAME LumenSurfaceCacheSmokeTest COMMAND LumenSurfaceCacheSmokeTest)

    message(STATUS "Configured Lumen surface-cache smoke-test target: LumenSurfaceCacheSmokeTest")
endif()

set_target_properties(LumenSceneGeometrySmokeTest PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

target_link_libraries(LumenSceneGeometrySmokeTest PRIVATE ZRuntime)

if(TARGET ZEnginePCH)
    target_link_libraries(LumenSceneGeometrySmokeTest PRIVATE ZEnginePCH)
    target_include_directories(LumenSceneGeometrySmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(LumenSceneGeometrySmokeTest ZEnginePCH)
    target_precompile_headers(LumenSceneGeometrySmokeTest REUSE_FROM ZEnginePCH)
endif()

if(MSVC)
    target_compile_options(LumenSceneGeometrySmokeTest PRIVATE /W4)
endif()

# Same Apple link fix as PlayerSettings/Test: libZRuntime.a references render
# symbols whose TUs are not built for Apple, so without the stubs the target
# cannot link on macOS at all.
if(APPLE)
    target_sources(LumenSceneGeometrySmokeTest PRIVATE
        ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
endif()

add_test(NAME LumenSceneGeometrySmokeTest COMMAND LumenSceneGeometrySmokeTest)

message(STATUS "Configured Lumen scene-geometry smoke-test target: LumenSceneGeometrySmokeTest")
