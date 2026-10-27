# =====================================================================
# ScriptTime smoke test (script-facing time queries):
#
#   ScriptTimeSmokeTest  -DZENGINE_BUILD_SCRIPT_TIME_SMOKE_TEST=ON
#
# Same "own main()" / side-target pattern as the other smoke tests: the source
# is carved out of the ZRuntime glob in Runtime/CMakeLists.txt so /WHOLEARCHIVE
# cannot drag its main() into every consumer executable (LNK2005).
#
#   cmake -B build -DZENGINE_BUILD_SCRIPT_TIME_SMOKE_TEST=ON
#   cmake --build build --target ScriptTimeSmokeTest
#   ctest --test-dir build -R ScriptTimeSmokeTest
#
# Pure CPU -- no GPU, no window, no script runtime. Every method under test is
# static, so the test drives TimeState::BeginFrame directly and asserts what
# Time.* would hand a script. The point is timeScale: it is the one script-
# writable piece of state that changes how fast the *world* advances, so a bug
# there shows up as "physics and scripts disagreeing about the frame length".
#
# Links ZRuntime (Time is a ZCLASS, so it needs Object's reflection
# registration), not header-only.
# =====================================================================

option(ZENGINE_BUILD_SCRIPT_TIME_SMOKE_TEST
       "Build the script time (Time.*) smoke-test executable"
       OFF)

if(ZENGINE_BUILD_SCRIPT_TIME_SMOKE_TEST)
    add_executable(ScriptTimeSmokeTest ScriptTimeSmokeTest.cpp)

    set_target_properties(ScriptTimeSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(ScriptTimeSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(ScriptTimeSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(ScriptTimeSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(ScriptTimeSmokeTest ZEnginePCH)
        target_precompile_headers(ScriptTimeSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    # macOS: libZRuntime.a references render-side symbols whose defining TUs are
    # not built for Apple; -force_load drags their callers in. This test never
    # touches the renderer, so aborting stubs stand in (same as the other smoke
    # tests).
    if(APPLE)
        target_sources(ScriptTimeSmokeTest PRIVATE
            ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
    endif()

    if(MSVC)
        target_compile_options(ScriptTimeSmokeTest PRIVATE /W4)
    endif()

    add_test(NAME ScriptTimeSmokeTest COMMAND ScriptTimeSmokeTest)
    message(STATUS "Configured script time smoke test: ScriptTimeSmokeTest")
endif()
