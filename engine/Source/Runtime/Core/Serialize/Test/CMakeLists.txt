# =====================================================================
# PR-SE2: Schema-evolution smoke test for `.zasset` SerializedFile I/O
# ---------------------------------------------------------------------
# Builds a standalone executable that validates PR-SE1's Stage 2 change
# to SerializedFile::ReadObject -- i.e. the SafeBinaryRead-backed
# schema-aware read path -- end to end.
#
# Default OFF -- no normal build is affected. Opt in with:
#
#   cmake -B build -DZENGINE_BUILD_SCHEMA_EVOLUTION_SMOKE_TEST=ON
#   cmake --build build --target SchemaEvolutionSmokeTest
#   ./build/.../SchemaEvolutionSmokeTest
#
# Why a separate target instead of CTest / GoogleTest:
#   - The engine doesn't ship a test harness yet (only `shader_lab_test`,
#     `dx12_bindless_smoke_test`, `vulkan_bindless_smoke_test` exist,
#     and they're all manually-invoked main() smoke tests too).
#     PR-SE2 isn't the right place to introduce a framework -- this
#     target's contract is "schema-evolution smoke test", not "design
#     a testing strategy for ZEngine".
#   - The test deliberately runs OUTSIDE the editor / launcher so it
#     can validate the read/write paths without dragging in the
#     window-system manager or the RHI.
# =====================================================================

option(ZENGINE_BUILD_SCHEMA_EVOLUTION_SMOKE_TEST
       "Build the SerializedFile schema-evolution smoke-test executable (PR-SE2)"
       OFF)

option(ZENGINE_BUILD_PPTR_SMOKE_TEST
       "Build the PPtr<T> round-trip smoke-test executable (PR-SE3a-refine)"
       OFF)

option(ZENGINE_BUILD_YAML_ROUND_TRIP_SMOKE_TEST
       "Build the YAML text-serialization round-trip smoke-test executable"
       OFF)

option(ZENGINE_BUILD_REFLECTION_CODEGEN_SMOKE_TEST
       "Build the ZHeaderTool reflection-codegen TypeTree/binary dump smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_SCHEMA_EVOLUTION_SMOKE_TEST AND NOT ZENGINE_BUILD_PPTR_SMOKE_TEST AND
   NOT ZENGINE_BUILD_YAML_ROUND_TRIP_SMOKE_TEST AND NOT ZENGINE_BUILD_REFLECTION_CODEGEN_SMOKE_TEST)
    return()
endif()

# ---------------------------------------------------------------------
# Helper: configure a smoke-test target identically to schema-evo's
# original wiring (PRIVATE link to ZRuntime, share PCH, /W4 on MSVC,
# Engine/Tests folder).
# ---------------------------------------------------------------------
function(zengine_add_smoke_test target_name source_file)
    # Optional TEST_ARGS <arg>... are passed on to the executable by add_test.
    # The bare executable is usually a dump / manual aid; the CTest gate should
    # run its assertions, so a target that has both registers with the flags.
    cmake_parse_arguments(ST "" "" "TEST_ARGS" ${ARGN})

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

    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4)
    endif()

    # macOS: libZRuntime.a currently has 15 unresolved render symbols on Apple
    # and -force_load drags their callers in. These smoke tests never reach the
    # renderer; resolve them with aborting stubs (see SmokeTestAppleLinkStubs.cpp).
    if(APPLE)
        target_sources(${target_name} PRIVATE SmokeTestAppleLinkStubs.cpp)
    endif()

    # Register with CTest so `ctest` (and CI) actually runs it. Every smoke test
    # returns EXIT_FAILURE on the first failed check, so the exit code is enough.
    # WORKING_DIRECTORY defaults to CMAKE_CURRENT_BINARY_DIR, which is where the
    # YAML round-trip test writes and removes its scratch scene file.
    add_test(NAME ${target_name} COMMAND ${target_name} ${ST_TEST_ARGS})
endfunction()

if(ZENGINE_BUILD_SCHEMA_EVOLUTION_SMOKE_TEST)
    zengine_add_smoke_test(SchemaEvolutionSmokeTest SchemaEvolutionSmokeTest.cpp)
    message(STATUS "Configured SerializedFile schema-evolution smoke-test target: SchemaEvolutionSmokeTest")
endif()

if(ZENGINE_BUILD_PPTR_SMOKE_TEST)
    zengine_add_smoke_test(PPtrSmokeTest PptrSmokeTest.cpp)
    message(STATUS "Configured PPtr round-trip smoke-test target: PPtrSmokeTest")
endif()

if(ZENGINE_BUILD_YAML_ROUND_TRIP_SMOKE_TEST)
    zengine_add_smoke_test(YamlRoundTripSmokeTest YamlRoundTripSmokeTest.cpp)
    message(STATUS "Configured YAML round-trip smoke-test target: YamlRoundTripSmokeTest")
endif()

if(ZENGINE_BUILD_REFLECTION_CODEGEN_SMOKE_TEST)
    # Bare runs are the migration dump (see the header of the .cpp); CI runs the
    # invariant checks instead: type registry, Behaviour 'enabled' serialization,
    # ZPROPERTY(meta=(...)) vs the live TypeTree, the ZFUNCTION binding tables /
    # generated .d.ts, the ZPROPERTY / ZSCRIPTPROPERTY accessor property tables,
    # Cast<T> vs dynamic_cast, and the JS-side Behaviour declaration vs the
    # properties TypeScriptComponent injects.
    zengine_add_smoke_test(ReflectionCodegenSmokeTest ReflectionCodegenSmokeTest.cpp
                           TEST_ARGS --check-registry --check-behaviour-enabled --check-property-meta
                                     --check-script-bindings --check-script-properties --check-cast
                                     --check-behaviour-binding)
    message(STATUS "Configured reflection-codegen smoke-test target: ReflectionCodegenSmokeTest")
endif()
