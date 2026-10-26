# =====================================================================
# Console variable file (cvar.save / cvar.load) smoke test.
#
# Same "own main()" / side-target pattern as AssetBundle/Test and
# Core/Serialize/Test: the source is carved out of the ZRuntime glob in
# Runtime/CMakeLists.txt so /WHOLEARCHIVE cannot drag its main() into every
# consumer executable (LNK2005).
#
#   cmake -B build -DZENGINE_BUILD_CONSOLE_VARIABLES_FILE_SMOKE_TEST=ON
#   cmake --build build --target ConsoleVariablesFileSmokeTest
#
# Pure file work -- no project, no window, no GPU -- so it runs headless on
# every host.
# =====================================================================

option(ZENGINE_BUILD_CONSOLE_VARIABLES_FILE_SMOKE_TEST
       "Build the console-variable save/load smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_CONSOLE_VARIABLES_FILE_SMOKE_TEST)
    return()
endif()

add_executable(ConsoleVariablesFileSmokeTest ConsoleVariablesFileSmokeTest.cpp)

set_target_properties(ConsoleVariablesFileSmokeTest PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

target_link_libraries(ConsoleVariablesFileSmokeTest PRIVATE ZRuntime)

if(TARGET ZEnginePCH)
    target_link_libraries(ConsoleVariablesFileSmokeTest PRIVATE ZEnginePCH)
    target_include_directories(ConsoleVariablesFileSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(ConsoleVariablesFileSmokeTest ZEnginePCH)
    target_precompile_headers(ConsoleVariablesFileSmokeTest REUSE_FROM ZEnginePCH)
endif()

if(MSVC)
    target_compile_options(ConsoleVariablesFileSmokeTest PRIVATE /W4)
endif()

# Same Apple link fix as AssetBundle/Test: libZRuntime.a references render
# symbols whose TUs are not built for Apple, so without the stubs the target
# cannot link on macOS at all.
if(APPLE)
    target_sources(ConsoleVariablesFileSmokeTest PRIVATE
        ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
endif()

add_test(NAME ConsoleVariablesFileSmokeTest COMMAND ConsoleVariablesFileSmokeTest)

message(STATUS "Configured console-variable file smoke-test target: ConsoleVariablesFileSmokeTest")
