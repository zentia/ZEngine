# RenderGraph smoke tests: one for the compiler (Setup/Compile), one for the executor
# (pool + Execute). Both own a main(), so neither may be swept into the ZRuntime glob
# (which links with /WHOLEARCHIVE on MSVC). See the matching list(REMOVE_ITEM ...) pairs
# in engine/Source/Runtime/CMakeLists.txt.
#
#   cmake -S . -B build -DZENGINE_BUILD_RENDERGRAPH_SMOKE_TEST=ON
#                       -DZENGINE_BUILD_RENDERGRAPH_EXECUTE_SMOKE_TEST=ON
#   cmake --build build --target RenderGraphSmokeTest RenderGraphExecuteSmokeTest

# The four graph translation units shared by both executables.
set(ZENGINE_RENDERGRAPH_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../RenderGraph.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../RenderGraphBuilder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../RenderGraphTypes.cpp
)

# -------------------------------------------------------------------------------------
# Compiler smoke test -- no RHI at all.
# -------------------------------------------------------------------------------------
option(ZENGINE_BUILD_RENDERGRAPH_SMOKE_TEST
       "Build the RenderGraph compiler smoke-test executable"
       OFF)

if(ZENGINE_BUILD_RENDERGRAPH_SMOKE_TEST)
    add_executable(RenderGraphSmokeTest
        RenderGraphSmokeTest.cpp
        ${ZENGINE_RENDERGRAPH_SOURCES}
    )

    set_target_properties(RenderGraphSmokeTest PROPERTIES
        CXX_STANDARD 20
        FOLDER "Engine/Tests"
    )

    target_include_directories(RenderGraphSmokeTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..
    )

    # Registered with CTest (exit code is the verdict: 1 on any failed check).
    add_test(NAME RenderGraphSmokeTest COMMAND RenderGraphSmokeTest)

    message(STATUS "Configured RenderGraph smoke-test target: RenderGraphSmokeTest")
endif()

# -------------------------------------------------------------------------------------
# Execute-half smoke test -- adds RenderGraphResources.cpp. Still no GPU and no RHI
# implementation: the executor only talks to IRGResourceDevice, and the RHI types are
# opaque pointers, so a stub device in the test is enough to observe everything.
# -------------------------------------------------------------------------------------
option(ZENGINE_BUILD_RENDERGRAPH_EXECUTE_SMOKE_TEST
       "Build the RenderGraph executor smoke-test executable"
       OFF)

if(ZENGINE_BUILD_RENDERGRAPH_EXECUTE_SMOKE_TEST)
    add_executable(RenderGraphExecuteSmokeTest
        RenderGraphExecuteSmokeTest.cpp
        ${ZENGINE_RENDERGRAPH_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/../RenderGraphResources.cpp
    )

    set_target_properties(RenderGraphExecuteSmokeTest PROPERTIES
        CXX_STANDARD 20
        FOLDER "Engine/Tests"
    )

    # RenderGraphResources.cpp includes RHI.h (for the ten backend signatures), and
    # RHI.h -> RenderType.h -> Core/Base/Hash.h -> <EASTL/functional.h>. The compiler
    # smoke test never sees that header, so it can stay truly dependency-free; this one
    # needs EASTL on the include path. Wired directly rather than through ZEnginePCH so
    # the executable stays a four-file build with no engine library linked.
    target_include_directories(RenderGraphExecuteSmokeTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..
        ${THIRD_PARTY_DIR}/puerts/unity/native/EASTL/include
    )
    target_compile_definitions(RenderGraphExecuteSmokeTest PRIVATE EASTL_OPENSOURCE=1)

    add_test(NAME RenderGraphExecuteSmokeTest COMMAND RenderGraphExecuteSmokeTest)

    message(STATUS "Configured RenderGraph execute smoke-test target: RenderGraphExecuteSmokeTest")
endif()
