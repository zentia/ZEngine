# =====================================================================
# Prefab end-to-end smoke test (doc/PrefabSystem_Design.md §9.4 items 1 + 2).
# ---------------------------------------------------------------------
# Save -> load from disk -> InstantiateAsPrefab -> override -> ApplyAll ->
# re-save + reload -> Revert -> PrefabInstance scene round-trip (binary +
# YAML) -> PrefabInstance through Level::save / LoadObjectGraph -> lazy
# non-header PPtr -> one file one identity. Links ZRuntime plus the Editor/Prefab TUs (they are not in
# libZRuntime). Default OFF; opt in with:
#
#   cmake -B build -DZENGINE_BUILD_PREFAB_E2E_SMOKE_TEST=ON
#   cmake --build build --target PrefabEndToEndSmokeTest
#
# Headless without a native toolchain: scripts/verify_prefab_e2e.sh builds and
# runs the same source against the web build's libZRuntime.a under node.
# =====================================================================

option(ZENGINE_BUILD_PREFAB_E2E_SMOKE_TEST
       "Build the prefab end-to-end (save/instantiate/apply/revert/scene) smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_PREFAB_E2E_SMOKE_TEST)
    return()
endif()

file(GLOB PREFAB_EDITOR_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../*.cpp)

# Editor-side TUs the test drives directly. The Prefab glob alone is not enough:
# AssetRegistry, UndoSystem and BuildDependencyWalk are separate editor
# subsystems, and without them the link fails with 14 unresolved externals.
add_executable(PrefabEndToEndSmokeTest
    PrefabEndToEndSmokeTest.cpp
    ${PREFAB_EDITOR_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Build/SceneCook.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Build/BuildDependencyWalk.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../AssetRegistry/AssetRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Undo/UndoSystem.cpp
)

set_target_properties(PrefabEndToEndSmokeTest PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

# "Editor/Prefab/..." is included engine-Source relative.
target_include_directories(PrefabEndToEndSmokeTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..
    ${ENGINE_ROOT_DIR}/Source
)

# Object.h derives from eastl types and ImmediatePtr.h calls TransferMetaFlags,
# neither of which is reachable without EASTL on the include path and the shared
# PCH -- the same wiring every other ZRuntime-linked smoke test uses. Without it
# this target fails with ~109 errors (eastl is not a class or namespace, ...).
target_include_directories(PrefabEndToEndSmokeTest PRIVATE
    ${THIRD_PARTY_DIR}/puerts/unity/native/EASTL/include
)
target_compile_definitions(PrefabEndToEndSmokeTest PRIVATE EASTL_OPENSOURCE=1)

target_link_libraries(PrefabEndToEndSmokeTest PRIVATE ZRuntime)

if(TARGET ZEnginePCH)
    target_link_libraries(PrefabEndToEndSmokeTest PRIVATE ZEnginePCH)
    target_include_directories(PrefabEndToEndSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(PrefabEndToEndSmokeTest ZEnginePCH)
    target_precompile_headers(PrefabEndToEndSmokeTest REUSE_FROM ZEnginePCH)
endif()

# Exit code is the verdict (0 pass, 1 a check failed, 77 bring-up failed).
add_test(NAME PrefabEndToEndSmokeTest COMMAND PrefabEndToEndSmokeTest)

message(STATUS "Configured prefab end-to-end smoke-test target: PrefabEndToEndSmokeTest")
