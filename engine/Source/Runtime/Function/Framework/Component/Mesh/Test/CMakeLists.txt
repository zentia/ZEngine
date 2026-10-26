# =====================================================================
# Runtime material instance smoke test.
#
# Same "own main()" / side-target pattern as Function/Input/Test and
# Core/Memory/Test: the source is carved out of the ZRuntime glob in
# Runtime/CMakeLists.txt so /WHOLEARCHIVE cannot drag its main() into every
# consumer executable (LNK2005).
#
#   cmake -B build -DZENGINE_BUILD_MATERIAL_INSTANCE_SMOKE_TEST=ON
#   cmake --build build --target MaterialInstanceOverrideSmokeTest
#
# Proves the CPU half of the runtime material instance feature: a per-sub-mesh
# override lands on **only** its own slot and flags it
# `m_UseMaterialInstance`, so RenderScene hands that part its own
# GpuPBRMaterial instead of the asset-shared one.
#
# Pure CPU -- no project, no window, no GPU, no AssetManager. The sub-meshes
# it builds carry no material asset on purpose: populateMaterialDesc returns
# before touching AssetManager when the asset path is empty, and
# SubmitRenderState early-returns while m_ParentObject is null. So it runs
# headless on every host (native macOS verified: 36 checks, exit 0).
#
# Unlike the EnhancedInput test this one is NOT header-only -- it constructs a
# BaseRenderer and reads the GameObjectPartDescs BuildRenderParts produces --
# so it links ZRuntime.
# =====================================================================

option(ZENGINE_BUILD_MATERIAL_INSTANCE_SMOKE_TEST
       "Build the runtime material instance smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_MATERIAL_INSTANCE_SMOKE_TEST)
    return()
endif()

add_executable(MaterialInstanceOverrideSmokeTest MaterialInstanceOverrideSmokeTest.cpp)

set_target_properties(MaterialInstanceOverrideSmokeTest PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

target_link_libraries(MaterialInstanceOverrideSmokeTest PRIVATE ZRuntime)

if(TARGET ZEnginePCH)
    target_link_libraries(MaterialInstanceOverrideSmokeTest PRIVATE ZEnginePCH)
    target_include_directories(MaterialInstanceOverrideSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(MaterialInstanceOverrideSmokeTest ZEnginePCH)
    target_precompile_headers(MaterialInstanceOverrideSmokeTest REUSE_FROM ZEnginePCH)
endif()

# macOS: libZRuntime.a references render-side symbols whose defining TUs are
# not built for Apple; -force_load drags their callers in. This test never
# touches the renderer, so aborting stubs stand in (same as the other smoke
# tests).
if(APPLE)
    target_sources(MaterialInstanceOverrideSmokeTest PRIVATE
        ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
endif()

if(MSVC)
    target_compile_options(MaterialInstanceOverrideSmokeTest PRIVATE /W4)
endif()

add_test(NAME MaterialInstanceOverrideSmokeTest COMMAND MaterialInstanceOverrideSmokeTest)

message(STATUS "Configured runtime material instance smoke test: MaterialInstanceOverrideSmokeTest")
