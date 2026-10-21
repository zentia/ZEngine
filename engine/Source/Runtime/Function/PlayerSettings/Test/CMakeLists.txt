# =====================================================================
# PlayerSettings smoke test.
#
# Same "own main()" / side-target pattern as AssetBundle/Test and
# Core/Serialize/Test: the source is carved out of the ZRuntime glob in
# Runtime/CMakeLists.txt so /WHOLEARCHIVE cannot drag its main() into every
# consumer executable (LNK2005).
#
#   cmake -B build -DZENGINE_BUILD_PLAYER_SETTINGS_SMOKE_TEST=ON
#   cmake --build build --target PlayerSettingsSmokeTest
#
# Pure file work -- no project, no window, no GPU -- so it runs headless on
# every host.
# =====================================================================

option(ZENGINE_BUILD_PLAYER_SETTINGS_SMOKE_TEST
       "Build the PlayerSettings smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_PLAYER_SETTINGS_SMOKE_TEST)
    return()
endif()

add_executable(PlayerSettingsSmokeTest PlayerSettingsSmokeTest.cpp)

set_target_properties(PlayerSettingsSmokeTest PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

target_link_libraries(PlayerSettingsSmokeTest PRIVATE ZRuntime)

if(TARGET ZEnginePCH)
    target_link_libraries(PlayerSettingsSmokeTest PRIVATE ZEnginePCH)
    target_include_directories(PlayerSettingsSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(PlayerSettingsSmokeTest ZEnginePCH)
    target_precompile_headers(PlayerSettingsSmokeTest REUSE_FROM ZEnginePCH)
endif()

if(MSVC)
    target_compile_options(PlayerSettingsSmokeTest PRIVATE /W4)
endif()

# Same Apple link fix as AssetBundle/Test: libZRuntime.a references render
# symbols whose TUs are not built for Apple, so without the stubs the target
# cannot link on macOS at all.
if(APPLE)
    target_sources(PlayerSettingsSmokeTest PRIVATE
        ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
endif()

add_test(NAME PlayerSettingsSmokeTest COMMAND PlayerSettingsSmokeTest)

message(STATUS "Configured PlayerSettings smoke-test target: PlayerSettingsSmokeTest")
