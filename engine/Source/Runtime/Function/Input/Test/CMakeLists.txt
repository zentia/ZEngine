# =====================================================================
# EnhancedInput frame-delta smoke test.
#
# Same "own main()" / side-target pattern as Console/Test and
# Core/Serialize/Test: the source is carved out of the ZRuntime glob in
# Runtime/CMakeLists.txt so /WHOLEARCHIVE cannot drag its main() into every
# consumer executable (LNK2005).
#
#   cmake -B build -DZENGINE_BUILD_ENHANCED_INPUT_DELTA_SMOKE_TEST=ON
#   cmake --build build --target EnhancedInputFrameDeltaSmokeTest
#
# Pure CPU — EnhancedInputSystem::ComputeFrameDelta + TriggerHold are both
# header-only (inline), and their include chain (Math/Vector2/SerializeUtility/
# EngineSystem/InputTriggers) is all header-only, so this target links nothing
# but the include root. No project, no window, no GPU — runs headless on every
# host (web via run_all_smoke_tests_web.py, native via wsl_verify_linux.sh).
# =====================================================================

option(ZENGINE_BUILD_ENHANCED_INPUT_DELTA_SMOKE_TEST
       "Build the EnhancedInput frame-delta smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_ENHANCED_INPUT_DELTA_SMOKE_TEST)
    return()
endif()

# Vector2::ZERO is declared in the header but defined out-of-line in Vector2.cpp,
# so this target is not purely header-only: without that one TU it links with
# LNK2001 on Windows. Nothing else from the runtime is needed.
add_executable(EnhancedInputFrameDeltaSmokeTest
    EnhancedInputFrameDeltaSmokeTest.cpp
    ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Math/Vector2.cpp
)

set_target_properties(EnhancedInputFrameDeltaSmokeTest PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

# Header-only include chain: only the engine/Source root is needed.
target_include_directories(EnhancedInputFrameDeltaSmokeTest PRIVATE
    ${ENGINE_ROOT_DIR}/Source
)

if(MSVC)
    target_compile_options(EnhancedInputFrameDeltaSmokeTest PRIVATE /W4)
endif()

add_test(NAME EnhancedInputFrameDeltaSmokeTest COMMAND EnhancedInputFrameDeltaSmokeTest)

message(STATUS "Configured EnhancedInput frame-delta smoke-test target: EnhancedInputFrameDeltaSmokeTest")
