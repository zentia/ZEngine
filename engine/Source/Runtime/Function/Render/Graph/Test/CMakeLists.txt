# RenderGraph compiler smoke test.
#
#   cmake -S . -B build -DZENGINE_BUILD_RENDERGRAPH_SMOKE_TEST=ON
#   cmake --build build --target RenderGraphSmokeTest
#
# Off by default: it owns a main(), so it must never be swept into the ZRuntime glob
# (which links with /WHOLEARCHIVE on MSVC). See the matching list(REMOVE_ITEM ...) pair
# in engine/Source/Runtime/CMakeLists.txt.
option(ZENGINE_BUILD_RENDERGRAPH_SMOKE_TEST
       "Build the RenderGraph compiler smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_RENDERGRAPH_SMOKE_TEST)
    return()
endif()

add_executable(RenderGraphSmokeTest
    RenderGraphSmokeTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../RenderGraph.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../RenderGraphBuilder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../RenderGraphTypes.cpp
)

set_target_properties(RenderGraphSmokeTest PROPERTIES
    CXX_STANDARD 20
    FOLDER "Engine/Tests"
)

target_include_directories(RenderGraphSmokeTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..
)

message(STATUS "Configured RenderGraph smoke-test target: RenderGraphSmokeTest")
