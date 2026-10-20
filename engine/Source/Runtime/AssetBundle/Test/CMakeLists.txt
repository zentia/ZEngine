# =====================================================================
# PR-PAK1: AssetBundle smoke test.
#
# Standalone executable for the .zbundle container + mounted-bundle
# runtime path, so it runs headless in CI with no window, no GPU and no
# project:
#
#   cmake -B build -DZENGINE_BUILD_ASSET_BUNDLE_SMOKE_TEST=ON
#   cmake --build build --target AssetBundleSmokeTest
#   ./build/.../AssetBundleSmokeTest
#
# Same "own main()" / side-target pattern as Core/Serialize/Test: the source is
# carved out of the ZRuntime glob in Runtime/CMakeLists.txt so /WHOLEARCHIVE
# cannot drag its main() into every consumer executable (LNK2005).
# =====================================================================

option(ZENGINE_BUILD_ASSET_BUNDLE_SMOKE_TEST
       "Build the AssetBundle smoke-test executable (PR-PAK1)"
       OFF)

if(NOT ZENGINE_BUILD_ASSET_BUNDLE_SMOKE_TEST)
    return()
endif()

add_executable(AssetBundleSmokeTest AssetBundleSmokeTest.cpp)

set_target_properties(AssetBundleSmokeTest PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

target_link_libraries(AssetBundleSmokeTest PRIVATE ZRuntime)

if(TARGET ZEnginePCH)
    target_link_libraries(AssetBundleSmokeTest PRIVATE ZEnginePCH)
    target_include_directories(AssetBundleSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(AssetBundleSmokeTest ZEnginePCH)
    target_precompile_headers(AssetBundleSmokeTest REUSE_FROM ZEnginePCH)
endif()

if(MSVC)
    target_compile_options(AssetBundleSmokeTest PRIVATE /W4)
endif()

# Same Apple link fix as Core/Serialize/Test: libZRuntime.a references render
# symbols whose TUs are not built for Apple, and -force_load drags them in even
# though this test never touches the renderer. Without this the target cannot
# link on macOS at all, which silently disabled every assertion below there.
if(APPLE)
    target_sources(AssetBundleSmokeTest PRIVATE
        ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
endif()

# Registered with CTest; the test returns EXIT_FAILURE on the first failed check.
add_test(NAME AssetBundleSmokeTest COMMAND AssetBundleSmokeTest)

message(STATUS "Configured AssetBundle smoke-test target: AssetBundleSmokeTest")
