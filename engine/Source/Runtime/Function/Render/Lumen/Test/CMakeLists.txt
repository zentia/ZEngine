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

# ---------------------------------------------------------------------
# Global-illumination smoke test. Pins that ComputeGlobalIllumination
# fills a world-space irradiance probe grid (it used to be a TODO that
# returned true and did nothing), that the grid is anchored to the
# distance field rather than the camera, that probe updates are
# budgeted, that temporal accumulation actually lags behind a change,
# and that the colour comes from the surface cache.
# ---------------------------------------------------------------------
option(ZENGINE_BUILD_LUMEN_GLOBAL_ILLUMINATION_SMOKE_TEST
       "Build the Lumen global-illumination smoke-test executable"
       OFF)

if(ZENGINE_BUILD_LUMEN_GLOBAL_ILLUMINATION_SMOKE_TEST)
    add_executable(LumenGlobalIlluminationSmokeTest LumenGlobalIlluminationSmokeTest.cpp)

    set_target_properties(LumenGlobalIlluminationSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(LumenGlobalIlluminationSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(LumenGlobalIlluminationSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(LumenGlobalIlluminationSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(LumenGlobalIlluminationSmokeTest ZEnginePCH)
        target_precompile_headers(LumenGlobalIlluminationSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(LumenGlobalIlluminationSmokeTest PRIVATE /W4)
    endif()

    if(APPLE)
        target_sources(LumenGlobalIlluminationSmokeTest PRIVATE
            ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
    endif()

    add_test(NAME LumenGlobalIlluminationSmokeTest COMMAND LumenGlobalIlluminationSmokeTest)

    message(STATUS "Configured Lumen global-illumination smoke-test target: LumenGlobalIlluminationSmokeTest")
endif()

# ---------------------------------------------------------------------
# Render-pass wiring smoke test. Pins that LumenRenderPass::Render
# actually publishes its irradiance into RenderScene (it used to end in
# a TODO with zero callers), that the r.Lumen.* CVars are registered and
# live, and that RenderPipelineBase::UpdateLumen dispatches to a Lumen
# pass while ignoring anything else.
# ---------------------------------------------------------------------
option(ZENGINE_BUILD_LUMEN_RENDER_PASS_WIRING_SMOKE_TEST
       "Build the Lumen render-pass wiring smoke-test executable"
       OFF)

if(ZENGINE_BUILD_LUMEN_RENDER_PASS_WIRING_SMOKE_TEST)
    add_executable(LumenRenderPassWiringSmokeTest LumenRenderPassWiringSmokeTest.cpp)

    set_target_properties(LumenRenderPassWiringSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(LumenRenderPassWiringSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(LumenRenderPassWiringSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(LumenRenderPassWiringSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(LumenRenderPassWiringSmokeTest ZEnginePCH)
        target_precompile_headers(LumenRenderPassWiringSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(LumenRenderPassWiringSmokeTest PRIVATE /W4)
    endif()

    if(APPLE)
        target_sources(LumenRenderPassWiringSmokeTest PRIVATE
            ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
    endif()

    add_test(NAME LumenRenderPassWiringSmokeTest COMMAND LumenRenderPassWiringSmokeTest)

    message(STATUS "Configured Lumen render-pass wiring smoke-test target: LumenRenderPassWiringSmokeTest")
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
