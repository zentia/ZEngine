# =====================================================================
# Nanite cluster-builder smoke test.
#
# Own main(); carved out of the ZRuntime glob in Runtime/CMakeLists.txt so its
# main() cannot be dragged into every consumer executable (LNK2005).
#
#   cmake -B build -DZENGINE_BUILD_NANITE_CLUSTER_SMOKE_TEST=ON
#   cmake --build build --target NaniteClusterBuilderSmokeTest
#
# Pure CPU work -- no RHI, no window, no project. Unlike the Lumen tests this
# one is NOT gated on the Vulkan/Emscripten conditions: Function/Render/Nanite
# is never stripped from ZRuntime, so the test links everywhere ZRuntime builds.
# =====================================================================

option(ZENGINE_BUILD_NANITE_CLUSTER_SMOKE_TEST
       "Build the Nanite cluster-builder smoke-test executable"
       OFF)

if(ZENGINE_BUILD_NANITE_CLUSTER_SMOKE_TEST)
    add_executable(NaniteClusterBuilderSmokeTest NaniteClusterBuilderSmokeTest.cpp)

    set_target_properties(NaniteClusterBuilderSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(NaniteClusterBuilderSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(NaniteClusterBuilderSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(NaniteClusterBuilderSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(NaniteClusterBuilderSmokeTest ZEnginePCH)
        target_precompile_headers(NaniteClusterBuilderSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(NaniteClusterBuilderSmokeTest PRIVATE /W4)
    endif()

    if(APPLE)
        target_sources(NaniteClusterBuilderSmokeTest PRIVATE
            ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
    endif()

    add_test(NAME NaniteClusterBuilderSmokeTest COMMAND NaniteClusterBuilderSmokeTest)

    message(STATUS "Configured Nanite cluster-builder smoke test: NaniteClusterBuilderSmokeTest")
endif()
