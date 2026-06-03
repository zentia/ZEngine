# =====================================================================
# Derived Data Cache smoke test (texture cook, Phase 3).
# ---------------------------------------------------------------------
# Standalone executable that round-trips the LMDB-backed DDC + accessor.
# Default OFF; opt in with:
#
#   cmake -B build -DZENGINE_BUILD_DDC_SMOKE_TEST=ON
#   cmake --build build --target DerivedDataCacheSmokeTest
#
# PRIVATE link to ZRuntime (DDC sources + lmdb propagate transitively).
# =====================================================================

option(ZENGINE_BUILD_DDC_SMOKE_TEST
       "Build the Derived Data Cache smoke-test executable (texture cook P3)"
       OFF)

if(NOT ZENGINE_BUILD_DDC_SMOKE_TEST)
    return()
endif()

add_executable(DerivedDataCacheSmokeTest DerivedDataCacheSmokeTest.cpp)

set_target_properties(DerivedDataCacheSmokeTest PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

target_link_libraries(DerivedDataCacheSmokeTest PRIVATE ZRuntime)

if(TARGET ZEnginePCH)
    target_link_libraries(DerivedDataCacheSmokeTest PRIVATE ZEnginePCH)
    target_include_directories(DerivedDataCacheSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(DerivedDataCacheSmokeTest ZEnginePCH)
    target_precompile_headers(DerivedDataCacheSmokeTest REUSE_FROM ZEnginePCH)
endif()

if(MSVC)
    target_compile_options(DerivedDataCacheSmokeTest PRIVATE /W3)
endif()

message(STATUS "Configured Derived Data Cache smoke-test target: DerivedDataCacheSmokeTest")
