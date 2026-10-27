# =====================================================================
# ScriptInput smoke test (script-facing input queries):
#
#   ScriptInputSmokeTest  -DZENGINE_BUILD_SCRIPT_INPUT_SMOKE_TEST=ON
#
# Same "own main()" / side-target pattern as the other smoke tests: the source
# is carved out of the ZRuntime glob in Runtime/CMakeLists.txt so /WHOLEARCHIVE
# cannot drag its main() into every consumer executable (LNK2005).
#
#   cmake -B build -DZENGINE_BUILD_SCRIPT_INPUT_SMOKE_TEST=ON
#   cmake --build build --target ScriptInputSmokeTest
#   ctest --test-dir build -R ScriptInputSmokeTest
#
# Pure CPU -- no GPU, no window, no script runtime. The test constructs a real
# EnhancedInputSystem but deliberately does NOT call Initialize() (that would
# register GLFW callbacks with WindowSystem), so it exercises the key-state
# queries -- held / press-edge / release-edge -- by injecting OnKey /
# OnMouseButton / OnScroll / OnTouch by hand and ticking.
#
# The point is the press edge: it must live exactly one frame, and REPEAT must
# not count as a new press. Get those wrong and "jump once per Space press"
# silently becomes "jump every frame while Space is held".
#
# Links ZRuntime (Input is a ZCLASS, so it needs Object's reflection
# registration).
# =====================================================================

option(ZENGINE_BUILD_SCRIPT_INPUT_SMOKE_TEST
       "Build the script input (Input.*) smoke-test executable"
       OFF)

if(ZENGINE_BUILD_SCRIPT_INPUT_SMOKE_TEST)
    add_executable(ScriptInputSmokeTest ScriptInputSmokeTest.cpp)

    set_target_properties(ScriptInputSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(ScriptInputSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(ScriptInputSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(ScriptInputSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(ScriptInputSmokeTest ZEnginePCH)
        target_precompile_headers(ScriptInputSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    # macOS: libZRuntime.a references render-side symbols whose defining TUs are
    # not built for Apple; -force_load drags their callers in. This test never
    # touches the renderer, so aborting stubs stand in (same as the other smoke
    # tests).
    if(APPLE)
        target_sources(ScriptInputSmokeTest PRIVATE
            ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
    endif()

    if(MSVC)
        target_compile_options(ScriptInputSmokeTest PRIVATE /W4)
    endif()

    add_test(NAME ScriptInputSmokeTest COMMAND ScriptInputSmokeTest)
    message(STATUS "Configured script input smoke test: ScriptInputSmokeTest")
endif()
