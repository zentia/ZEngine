# =====================================================================
# ScriptMath smoke test (script vector / quaternion / scalar math):
#
#   ScriptMathSmokeTest  -DZENGINE_BUILD_SCRIPT_MATH_SMOKE_TEST=ON
#
# Same "own main()" / side-target pattern as the other smoke tests: the source
# is carved out of the ZRuntime glob in Runtime/CMakeLists.txt so /WHOLEARCHIVE
# cannot drag its main() into every consumer executable (LNK2005).
#
#   cmake -B build -DZENGINE_BUILD_SCRIPT_MATH_SMOKE_TEST=ON
#   cmake --build build --target ScriptMathSmokeTest
#   ctest --test-dir build -R ScriptMathSmokeTest
#
# Pure CPU -- no GPU, no window, and notably **no script runtime**: every method
# under test is static, so the test calls VectorMath::MoveTowards(...) and
# friends directly. That is the point -- it is the only part of the script-facing
# API that can be executed without a JS engine, which makes it the only guard
# that can actually fail when the arithmetic is wrong.
#
# Links ZRuntime (ScriptMath's classes are ZCLASS, so they need Object's
# reflection registration), not header-only like the EnhancedInput one.
# =====================================================================

option(ZENGINE_BUILD_SCRIPT_MATH_SMOKE_TEST
       "Build the script math (VectorMath/QuaternionMath/Mathf) smoke-test executable"
       OFF)

if(ZENGINE_BUILD_SCRIPT_MATH_SMOKE_TEST)
    add_executable(ScriptMathSmokeTest ScriptMathSmokeTest.cpp)

    set_target_properties(ScriptMathSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(ScriptMathSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(ScriptMathSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(ScriptMathSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(ScriptMathSmokeTest ZEnginePCH)
        target_precompile_headers(ScriptMathSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    # macOS: libZRuntime.a references render-side symbols whose defining TUs are
    # not built for Apple; -force_load drags their callers in. This test never
    # touches the renderer, so aborting stubs stand in (same as the other smoke
    # tests).
    if(APPLE)
        target_sources(ScriptMathSmokeTest PRIVATE
            ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
    endif()

    if(MSVC)
        target_compile_options(ScriptMathSmokeTest PRIVATE /W4)
    endif()

    add_test(NAME ScriptMathSmokeTest COMMAND ScriptMathSmokeTest)
    message(STATUS "Configured script math smoke test: ScriptMathSmokeTest")
endif()
