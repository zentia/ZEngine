# =====================================================================
# Addressables async-loading smoke test.
# ---------------------------------------------------------------------
# Standalone executable that exercises AsyncLoadManager's two-stage load
# (worker-thread file IO, game-thread object hydration) and its error
# semantics. Default OFF; opt in with:
#
#   cmake -B build -DZENGINE_BUILD_ADDRESSABLES_SMOKE_TEST=ON
#   cmake --build build --target AddressablesSmokeTest
# =====================================================================

option(ZENGINE_BUILD_ADDRESSABLES_SMOKE_TEST
       "Build the Addressables async-loading smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_ADDRESSABLES_SMOKE_TEST)
    return()
endif()

add_executable(AddressablesSmokeTest AddressablesSmokeTest.cpp)

set_target_properties(AddressablesSmokeTest PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

target_link_libraries(AddressablesSmokeTest PRIVATE ZRuntime)

if(TARGET ZEnginePCH)
    target_link_libraries(AddressablesSmokeTest PRIVATE ZEnginePCH)
    target_include_directories(AddressablesSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(AddressablesSmokeTest ZEnginePCH)
    target_precompile_headers(AddressablesSmokeTest REUSE_FROM ZEnginePCH)
endif()

if(MSVC)
    target_compile_options(AddressablesSmokeTest PRIVATE /W3)
endif()

# Registered with CTest; the test returns EXIT_FAILURE on the first failed check.
add_test(NAME AddressablesSmokeTest COMMAND AddressablesSmokeTest)

message(STATUS "Configured Addressables smoke-test target: AddressablesSmokeTest")
