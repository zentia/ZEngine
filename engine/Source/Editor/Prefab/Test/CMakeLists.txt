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

add_executable(PrefabEndToEndSmokeTest
    PrefabEndToEndSmokeTest.cpp
    ${PREFAB_EDITOR_SOURCES}
)

set_target_properties(PrefabEndToEndSmokeTest PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

# "Editor/Prefab/..." is included engine-Source relative.
target_include_directories(PrefabEndToEndSmokeTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..
)

target_link_libraries(PrefabEndToEndSmokeTest PRIVATE ZRuntime)

# Exit code is the verdict (0 pass, 1 a check failed, 77 bring-up failed).
add_test(NAME PrefabEndToEndSmokeTest COMMAND PrefabEndToEndSmokeTest)

message(STATUS "Configured prefab end-to-end smoke-test target: PrefabEndToEndSmokeTest")
