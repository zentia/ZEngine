# =====================================================================
# PR-A1: audio DSP smoke test.
#
# Builds a standalone executable that exercises the decoder + mixer with no
# engine, no window and no audio device, so it is safe for headless CI:
#
#   cmake -B build -DZENGINE_BUILD_AUDIO_SMOKE_TEST=ON
#   cmake --build build --target AudioMixerSmokeTest
#   ./build/.../AudioMixerSmokeTest
#
# Same "own main()" / side-target pattern as Core/Serialize/Test: the source is
# carved out of the ZRuntime glob in Runtime/CMakeLists.txt so /WHOLEARCHIVE
# cannot drag its main() into every consumer executable (LNK2005).
# =====================================================================

option(ZENGINE_BUILD_AUDIO_SMOKE_TEST
       "Build the audio decoder/mixer smoke-test executable (PR-A1)"
       OFF)

if(NOT ZENGINE_BUILD_AUDIO_SMOKE_TEST)
    return()
endif()

add_executable(AudioMixerSmokeTest AudioMixerSmokeTest.cpp)

set_target_properties(AudioMixerSmokeTest PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

target_link_libraries(AudioMixerSmokeTest PRIVATE ZRuntime)

if(TARGET ZEnginePCH)
    target_link_libraries(AudioMixerSmokeTest PRIVATE ZEnginePCH)
    target_include_directories(AudioMixerSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(AudioMixerSmokeTest ZEnginePCH)
    target_precompile_headers(AudioMixerSmokeTest REUSE_FROM ZEnginePCH)
endif()

if(MSVC)
    target_compile_options(AudioMixerSmokeTest PRIVATE /W4)
endif()

message(STATUS "Configured audio DSP smoke-test target: AudioMixerSmokeTest")
