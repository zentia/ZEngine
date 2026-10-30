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

# 这里**不能**用文件级 `if(NOT ...) return()`：本文件后面还有 6 个 option，
# return() 会把它们连同 target 一起压死（曾经就是这样：只开后面某个选项时
# CMake 报 "Manually-specified variables were not used"，测试静默不跑）。
# LumenSceneGeometrySmokeTest 的 add_executable 在这里，而它的属性 / 链接 /
# 注册段在**文件末尾**（因为要等 PCH / Apple stubs 的判定），两处都用
# 正向 `if()` 包起来，等价于原来的 return() 语义但不影响后面的选项。
if(ZENGINE_BUILD_LUMEN_SCENE_GEOMETRY_SMOKE_TEST)
    add_executable(LumenSceneGeometrySmokeTest LumenSceneGeometrySmokeTest.cpp)
endif()

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

# ---------------------------------------------------------------------
# Lumen -> per-frame uniform bridge smoke test. Pins the ONLY path from
# the CPU probe grid to a pixel: RenderScene::m_IndirectLighting is
# copied into MainCameraPerFrame::lumen_indirect (weight + colour, so a
# legitimately black scene is not read as "no data"), for both viewport
# slots, and that the hand-maintained GLSL/HLSL mirrors of the struct
# declare the field in the same place. The shaders are never compiled
# here, so a mirror left behind would otherwise be invisible.
# ---------------------------------------------------------------------
option(ZENGINE_BUILD_LUMEN_PER_FRAME_BRIDGE_SMOKE_TEST
       "Build the Lumen per-frame bridge smoke-test executable"
       OFF)

if(ZENGINE_BUILD_LUMEN_PER_FRAME_BRIDGE_SMOKE_TEST)
    add_executable(LumenPerFrameBridgeSmokeTest LumenPerFrameBridgeSmokeTest.cpp)

    set_target_properties(LumenPerFrameBridgeSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(LumenPerFrameBridgeSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(LumenPerFrameBridgeSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(LumenPerFrameBridgeSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(LumenPerFrameBridgeSmokeTest ZEnginePCH)
        target_precompile_headers(LumenPerFrameBridgeSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    # The mirror checks read engine/shader/* and the two UpdatePerFrameBuffer
    # implementations as text (none of them compiles on Apple). CMake hands the
    # test the absolute engine root so it does not depend on the working
    # directory.
    if(DEFINED ENGINE_ROOT_DIR)
        target_compile_definitions(LumenPerFrameBridgeSmokeTest PRIVATE
            "ZENGINE_ENGINE_ROOT_DIR=\"${ENGINE_ROOT_DIR}\"")
    endif()

    if(MSVC)
        target_compile_options(LumenPerFrameBridgeSmokeTest PRIVATE /W4)
    endif()

    if(APPLE)
        target_sources(LumenPerFrameBridgeSmokeTest PRIVATE
            ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
    endif()

    add_test(NAME LumenPerFrameBridgeSmokeTest COMMAND LumenPerFrameBridgeSmokeTest)

    message(STATUS "Configured Lumen per-frame bridge smoke test: LumenPerFrameBridgeSmokeTest")
endif()

# ---------------------------------------------------------------------
# Lumen reflections smoke test. LumenRenderer::ComputeReflections was an
# empty TODO, so ComputeReflection had zero callers and
# LumenReflectionData never left the Lumen directory. This pins the
# computation (screen-space reflections used to steal the result via a
# constant max_trace_distance branch; the mirror direction was dropped
# by the blend; roughness dimmed the radiance to black instead of
# spreading the ray) and the budgeted + temporally blended accumulation
# over the surface-cache pages.
# ---------------------------------------------------------------------
option(ZENGINE_BUILD_LUMEN_REFLECTIONS_SMOKE_TEST
       "Build the Lumen reflections smoke-test executable"
       OFF)

if(ZENGINE_BUILD_LUMEN_REFLECTIONS_SMOKE_TEST)
    add_executable(LumenReflectionsSmokeTest LumenReflectionsSmokeTest.cpp)

    set_target_properties(LumenReflectionsSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(LumenReflectionsSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(LumenReflectionsSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(LumenReflectionsSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(LumenReflectionsSmokeTest ZEnginePCH)
        target_precompile_headers(LumenReflectionsSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(LumenReflectionsSmokeTest PRIVATE /W4)
    endif()

    if(APPLE)
        target_sources(LumenReflectionsSmokeTest PRIVATE
            ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
    endif()

    add_test(NAME LumenReflectionsSmokeTest COMMAND LumenReflectionsSmokeTest)

    message(STATUS "Configured Lumen reflections smoke test: LumenReflectionsSmokeTest")
endif()

# LumenSceneGeometrySmokeTest 的收尾段（add_executable 在文件开头，同样的
# 正向守卫）。
if(ZENGINE_BUILD_LUMEN_SCENE_GEOMETRY_SMOKE_TEST)
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
endif()
