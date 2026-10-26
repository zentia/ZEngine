# =====================================================================
# P0-6: network layer smoke test.
#
# Builds a standalone executable that exercises HTTP (blocking + async),
# WebSocket and UDP against loopback only, so it is safe for headless CI:
#
#   cmake -B build -DZENGINE_BUILD_NETWORK_SMOKE_TEST=ON
#   cmake --build build --target NetworkSmokeTest
#   ctest -R NetworkSmokeTest
#
# Same "own main()" / side-target pattern as Function/Audio/Test: the source is
# carved out of the ZRuntime glob in Runtime/CMakeLists.txt so /WHOLEARCHIVE
# cannot drag its main() into every consumer executable (LNK2005).
#
# A curl-less build (Web/Emscripten) builds and passes too: the transports
# report NetworkStatus::Unsupported and the test accepts that as the expected
# outcome.
# =====================================================================

option(ZENGINE_BUILD_NETWORK_SMOKE_TEST
       "Build the network layer smoke-test executable (P0-6)"
       OFF)

if(NOT ZENGINE_BUILD_NETWORK_SMOKE_TEST)
    return()
endif()

add_executable(NetworkSmokeTest NetworkSmokeTest.cpp)

set_target_properties(NetworkSmokeTest PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

target_link_libraries(NetworkSmokeTest PRIVATE ZRuntime)

if(TARGET ZEnginePCH)
    target_link_libraries(NetworkSmokeTest PRIVATE ZEnginePCH)
    target_include_directories(NetworkSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(NetworkSmokeTest ZEnginePCH)
    target_precompile_headers(NetworkSmokeTest REUSE_FROM ZEnginePCH)
endif()

if(MSVC)
    target_compile_options(NetworkSmokeTest PRIVATE /W4)
endif()

# Registered with CTest; the test returns EXIT_FAILURE on any failed check.
# Offline by construction: nothing here leaves the machine unless
# ZENGINE_NETWORK_SMOKE_HTTP_URL is set in the environment.
add_test(NAME NetworkSmokeTest COMMAND NetworkSmokeTest)

message(STATUS "Configured network layer smoke-test target: NetworkSmokeTest")
