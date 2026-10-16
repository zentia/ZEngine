# =====================================================================
# AI layer smoke test: blackboard + behaviour tree semantics
# ---------------------------------------------------------------------
# Runs completely headless -- no WorldManager, no physics scene, no RHI.
# That is intentional and is the point of the test: the AI decision layer
# must be exercisable without booting the engine, which is what makes it
# testable in CI and debuggable outside the editor.
#
# Default OFF -- no normal build is affected. Opt in with:
#
#   cmake -B build -DZENGINE_BUILD_AI_SMOKE_TEST=ON
#   cmake --build build --target AISmokeTest
#   ./build/.../AISmokeTest
# =====================================================================

option(ZENGINE_BUILD_AI_SMOKE_TEST
       "Build the AI layer (blackboard + behaviour tree) smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_AI_SMOKE_TEST)
    return()
endif()

add_executable(AISmokeTest AISmokeTest.cpp)

set_target_properties(AISmokeTest PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

# Links ZRuntime for EASTL + math types only. The AI translation units are
# pulled in via /WHOLEARCHIVE on ZRuntime (see the static lib's INTERFACE
# link options), which is also what keeps reflection registration intact.
target_link_libraries(AISmokeTest PRIVATE ZRuntime)

if(TARGET ZEnginePCH)
    target_link_libraries(AISmokeTest PRIVATE ZEnginePCH)
    target_include_directories(AISmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(AISmokeTest ZEnginePCH)
    target_precompile_headers(AISmokeTest REUSE_FROM ZEnginePCH)
endif()

if(MSVC)
    target_compile_options(AISmokeTest PRIVATE /W4)
endif()

message(STATUS "Configured AI layer smoke-test target: AISmokeTest")

# ---------------------------------------------------------------------
# Navigation grid test: build / standability / pathing on a synthetic
# Z-up level.
#
# Separate from AISmokeTest because that one never touches NavMesh: it
# passed while the grid was built in the wrong plane. This target is the
# regression guard for the geometry itself.
# ---------------------------------------------------------------------
add_executable(NavMeshTest NavMeshTest.cpp)

set_target_properties(NavMeshTest PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

target_link_libraries(NavMeshTest PRIVATE ZRuntime)

if(TARGET ZEnginePCH)
    target_link_libraries(NavMeshTest PRIVATE ZEnginePCH)
    target_include_directories(NavMeshTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(NavMeshTest ZEnginePCH)
    target_precompile_headers(NavMeshTest REUSE_FROM ZEnginePCH)
endif()

if(MSVC)
    target_compile_options(NavMeshTest PRIVATE /W4)
endif()

# Real-level coverage: run the navmesh over the shipped 1-1 level, not only
# synthetic boxes. This is what catches wrong assumptions about actual asset
# scale, wall height and floor extents.
set(_nav_level_path "${Z_ROOT_DIR}/bin/$<CONFIG>/asset/level/1-1.level.json")
string(REPLACE "\\" "/" _nav_level_path "${_nav_level_path}")
set(_nav_binary_dir "${Z_ROOT_DIR}/bin/$<CONFIG>")
string(REPLACE "\\" "/" _nav_binary_dir "${_nav_binary_dir}")

target_compile_definitions(NavMeshTest PRIVATE
    ZENGINE_TEST_LEVEL_PATH="${_nav_level_path}"
    ZENGINE_TEST_BINARY_DIR="${_nav_binary_dir}"
)

message(STATUS "Configured navigation test target: NavMeshTest")

# ---------------------------------------------------------------------
# SceneGenTool: emits a .scene (YAML) level from the authored legacy
# .level.json.
#
# Levels are object graphs now (see LevelRes); the legacy .level.json is
# read only by this tool so its placements can be re-emitted as a .scene
# using the engine's own WriteObjectsToYaml, keeping the on-disk format
# from drifting from what Level::load() expects.
# ---------------------------------------------------------------------
add_executable(SceneGenTool SceneGenTool.cpp)

set_target_properties(SceneGenTool PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

target_link_libraries(SceneGenTool PRIVATE ZRuntime)

if(TARGET ZEnginePCH)
    target_link_libraries(SceneGenTool PRIVATE ZEnginePCH)
    target_include_directories(SceneGenTool PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(SceneGenTool ZEnginePCH)
    target_precompile_headers(SceneGenTool REUSE_FROM ZEnginePCH)
endif()

if(MSVC)
    target_compile_options(SceneGenTool PRIVATE /W4)
endif()

message(STATUS "Configured scene generator target: SceneGenTool")

# ---------------------------------------------------------------------
# AssetMigrator: retires the legacy JSON asset format by re-serializing
# every convertible .json asset as a binary .zasset, using the engine's
# own reader + writer so the encoding cannot drift.
#
# JSON cannot represent PPtr's (m_FileID, m_PathID) pairs, so it is being
# dropped in favour of binary (.zasset) and YAML authoring assets -- the
# same two representations Unity ships.
# ---------------------------------------------------------------------
add_executable(AssetMigrator AssetMigrator.cpp)

set_target_properties(AssetMigrator PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

target_link_libraries(AssetMigrator PRIVATE ZRuntime)

if(TARGET ZEnginePCH)
    target_link_libraries(AssetMigrator PRIVATE ZEnginePCH)
    target_include_directories(AssetMigrator PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(AssetMigrator ZEnginePCH)
    target_precompile_headers(AssetMigrator REUSE_FROM ZEnginePCH)
endif()

if(MSVC)
    target_compile_options(AssetMigrator PRIVATE /W4)
endif()

message(STATUS "Configured asset migration target: AssetMigrator")

add_executable(ComponentRegistryCheck ComponentRegistryCheck.cpp)
target_link_libraries(ComponentRegistryCheck PRIVATE ZRuntime)
if(TARGET ZEnginePCH)
    target_link_libraries(ComponentRegistryCheck PRIVATE ZEnginePCH)
    target_include_directories(ComponentRegistryCheck PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(ComponentRegistryCheck ZEnginePCH)
    target_precompile_headers(ComponentRegistryCheck REUSE_FROM ZEnginePCH)
endif()

if(MSVC)
    target_compile_options(ComponentRegistryCheck PRIVATE /W4)
endif()

message(STATUS "Configured component registry check target: ComponentRegistryCheck")