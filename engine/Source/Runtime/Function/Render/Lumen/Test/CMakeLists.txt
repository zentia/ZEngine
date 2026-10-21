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
