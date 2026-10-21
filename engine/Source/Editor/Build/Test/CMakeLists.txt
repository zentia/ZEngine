# =====================================================================
# BuildCache smoke test (incremental packaging state).
# ---------------------------------------------------------------------
# Standalone executable over the engine-agnostic cache (BuildCache.cpp has
# no engine dependency beyond rapidjson's headers), so it builds and runs
# without ZRuntime, a device or a project. Default OFF; opt in with:
#
#   cmake -B build -DZENGINE_BUILD_BUILD_CACHE_SMOKE_TEST=ON
#   cmake --build build --target BuildCacheSmokeTest
# =====================================================================

option(ZENGINE_BUILD_BUILD_CACHE_SMOKE_TEST
       "Build the editor BuildCache smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_BUILD_CACHE_SMOKE_TEST)
    return()
endif()

add_executable(BuildCacheSmokeTest
    BuildCacheSmokeTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../BuildCache.cpp
)

set_target_properties(BuildCacheSmokeTest PROPERTIES
    CXX_STANDARD 20
    FOLDER "Engine/Tests"
)

# "Editor/Build/BuildCache.h" is included engine/Source-relative; rapidjson is
# reached directly instead of through CommonPCH (which would drag the whole
# engine in for a test that needs none of it).
target_include_directories(BuildCacheSmokeTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../3rdparty/rapidjson/include
)

# Registered with CTest (exit code is the verdict: EXIT_FAILURE on any failed check).
add_test(NAME BuildCacheSmokeTest COMMAND BuildCacheSmokeTest)

message(STATUS "Configured BuildCache smoke-test target: BuildCacheSmokeTest")
