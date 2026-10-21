# RHI indirect-draw smoke test.
#
# Indirect draws are the one piece of the Nanite/Lumen GPU path whose correctness is
# a pure data contract: RHIDraw*IndirectCommand must lay out the way Vulkan, DX12 and
# Metal all read it. There is no GPU here and -- on Apple -- the Vulkan and DX12
# backends are not even compiled, so nothing else would catch a field drifting apart.
#
# Owns a main(), so it must never be swept into the ZRuntime glob (see the matching
# list(FILTER ...) in engine/Source/Runtime/CMakeLists.txt).
#
#   cmake -S . -B build -DZENGINE_BUILD_RHI_INDIRECT_DRAW_SMOKE_TEST=ON
#   cmake --build build --target RHIIndirectDrawSmokeTest

option(ZENGINE_BUILD_RHI_INDIRECT_DRAW_SMOKE_TEST
       "Build the RHI indirect-draw contract smoke-test executable"
       OFF)

if(ZENGINE_BUILD_RHI_INDIRECT_DRAW_SMOKE_TEST)
    add_executable(RHIIndirectDrawSmokeTest
        RHIIndirectDrawSmokeTest.cpp
    )

    set_target_properties(RHIIndirectDrawSmokeTest PROPERTIES
        CXX_STANDARD 20
        FOLDER "Engine/Tests"
    )

    # RHI.h -> RenderType.h -> Core/Base/Hash.h -> <EASTL/functional.h>, exactly as in
    # the RenderGraph execute test. Wired directly rather than through ZEnginePCH so the
    # executable stays a single-file build with no engine library linked.
    target_include_directories(RHIIndirectDrawSmokeTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..
        ${THIRD_PARTY_DIR}/puerts/unity/native/EASTL/include
    )
    target_compile_definitions(RHIIndirectDrawSmokeTest PRIVATE EASTL_OPENSOURCE=1)

    add_test(NAME RHIIndirectDrawSmokeTest COMMAND RHIIndirectDrawSmokeTest)

    message(STATUS "Configured RHI indirect-draw smoke-test target: RHIIndirectDrawSmokeTest")
endif()
