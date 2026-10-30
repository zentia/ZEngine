# =====================================================================
# Wwise backend smoke test (opt-in).
#
#   cmake -B build -DZENGINE_BUILD_WWISE_SMOKE_TEST=ON
#   cmake --build build --target WwiseIntegrationSmokeTest
#   ctest --test-dir build -C Debug -R WwiseIntegrationSmokeTest
#
#   WwiseIntegrationSmokeTest --with-device --bank-dir <GeneratedSoundBanks/Windows>
#
# Own main(); carved out of the ZRuntime GLOB_RECURSE in Runtime/CMakeLists.txt
# so /WHOLEARCHIVE cannot drag it into every consumer executable (LNK2005).
#
# Deliberately NOT added to the default CI matrix: group C needs an audio output
# device. Groups A and B (no fake support + wiring gate) need nothing and always
# run, which is what makes this worth having in the tree.
# =====================================================================

option(ZENGINE_BUILD_WWISE_SMOKE_TEST
       "Build the Wwise audio backend smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_WWISE_SMOKE_TEST)
    return()
endif()

add_executable(WwiseIntegrationSmokeTest WwiseIntegrationSmokeTest.cpp)

set_target_properties(WwiseIntegrationSmokeTest PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

# Needs ZRuntime: it drives the real SystemRegistry, and WwiseAudioSystem lives
# in the runtime. Its main() is carved out of the ZRuntime glob below.
target_link_libraries(WwiseIntegrationSmokeTest PRIVATE ZRuntime)
target_include_directories(WwiseIntegrationSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source)

if(TARGET ZEnginePCH)
    target_link_libraries(WwiseIntegrationSmokeTest PRIVATE ZEnginePCH)
    target_include_directories(WwiseIntegrationSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(WwiseIntegrationSmokeTest ZEnginePCH)
    target_precompile_headers(WwiseIntegrationSmokeTest REUSE_FROM ZEnginePCH)
endif()

if(MSVC)
    target_compile_options(WwiseIntegrationSmokeTest PRIVATE /W4)
endif()

# The test has to know whether *ZRuntime* was built with Wwise: ZENGINE_HAS_WWISE
# is PRIVATE to ZRuntime, so without this the test's own #ifdef disagrees with
# WwiseAudioSystem::IsCompiledIn() and group A reports a false mismatch.
if(TARGET WwiseSDK)
    target_link_libraries(WwiseIntegrationSmokeTest PRIVATE WwiseSDK)
endif()

target_compile_definitions(WwiseIntegrationSmokeTest PRIVATE
    "ZENGINE_ENGINE_ROOT_DIR=\"${ENGINE_ROOT_DIR}\"")

add_test(NAME WwiseIntegrationSmokeTest COMMAND WwiseIntegrationSmokeTest)
message(STATUS "Configured Wwise backend smoke-test target: WwiseIntegrationSmokeTest")
