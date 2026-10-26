# =====================================================================
# Runtime material smoke tests (two side targets):
#
#   MaterialInstanceOverrideSmokeTest  -DZENGINE_BUILD_MATERIAL_INSTANCE_SMOKE_TEST=ON
#       the 8 per-sub-mesh scalar overrides
#   MaterialCustomParamSmokeTest       -DZENGINE_BUILD_MATERIAL_CUSTOM_PARAM_SMOKE_TEST=ON
#       the named property block (customParams slots) + per-renderer keywords
#
# Same "own main()" / side-target pattern as Function/Input/Test and
# Core/Memory/Test: the sources are carved out of the ZRuntime glob in
# Runtime/CMakeLists.txt so /WHOLEARCHIVE cannot drag their main() into every
# consumer executable (LNK2005).
#
#   cmake -B build -DZENGINE_BUILD_MATERIAL_INSTANCE_SMOKE_TEST=ON \
#                  -DZENGINE_BUILD_MATERIAL_CUSTOM_PARAM_SMOKE_TEST=ON
#   cmake --build build --target MaterialInstanceOverrideSmokeTest MaterialCustomParamSmokeTest
#
# Pure CPU -- no project, no window, no GPU, no AssetManager. The sub-meshes
# they build carry no material asset on purpose: populateMaterialDesc returns
# before touching AssetManager when the asset path is empty, and
# SubmitRenderState early-returns while m_ParentObject is null. So they run
# headless on every host (native macOS verified, see doc/SMOKE_TESTS_HEADLESS_RUN.md).
#
# Unlike the EnhancedInput test these are NOT header-only -- they construct a
# BaseRenderer and read the GameObjectPartDescs BuildRenderParts produces --
# so they link ZRuntime.
# =====================================================================

option(ZENGINE_BUILD_MATERIAL_INSTANCE_SMOKE_TEST
       "Build the runtime material instance smoke-test executable"
       OFF)
option(ZENGINE_BUILD_MATERIAL_CUSTOM_PARAM_SMOKE_TEST
       "Build the runtime material property-block smoke-test executable"
       OFF)

function(zengine_add_mesh_smoke_test target_name source_file)
    add_executable(${target_name} ${source_file})

    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(${target_name} PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(${target_name} PRIVATE ZEnginePCH)
        target_include_directories(${target_name} PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(${target_name} ZEnginePCH)
        target_precompile_headers(${target_name} REUSE_FROM ZEnginePCH)
    endif()

    # macOS: libZRuntime.a references render-side symbols whose defining TUs are
    # not built for Apple; -force_load drags their callers in. These tests never
    # touch the renderer, so aborting stubs stand in (same as the other smoke
    # tests).
    if(APPLE)
        target_sources(${target_name} PRIVATE
            ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
    endif()

    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4)
    endif()

    add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

if(ZENGINE_BUILD_MATERIAL_INSTANCE_SMOKE_TEST)
    zengine_add_mesh_smoke_test(MaterialInstanceOverrideSmokeTest
                                MaterialInstanceOverrideSmokeTest.cpp)
    message(STATUS "Configured runtime material instance smoke test: MaterialInstanceOverrideSmokeTest")
endif()

if(ZENGINE_BUILD_MATERIAL_CUSTOM_PARAM_SMOKE_TEST)
    zengine_add_mesh_smoke_test(MaterialCustomParamSmokeTest
                                MaterialCustomParamSmokeTest.cpp)
    message(STATUS "Configured runtime property-block smoke test: MaterialCustomParamSmokeTest")
endif()
