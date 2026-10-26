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

# =====================================================================
# RHI 工厂注册表冒烟测试（阶段一·夯地基 第六落点 #1 + #4）
# ---------------------------------------------------------------------
# 验证 RHIFactory 注册表重构（#4）与 Linux 注册 RHI（#1）：每个编译进来的
# 后端自注册进工厂、IsAPIAvailable/GetDefaultAPI/CreateRHI 行为正确、未知 api
# 退回默认后端而非无限递归。无头实例化后端（构造默认、析构空桩），无需 GPU。
#
# 与 RHIIndirectDrawSmokeTest 不同：本测试要真正构造后端，故必须链接 ZRuntime
# （拿到 RHIFactory.cpp + 各后端 RHIBackendRegistrar + Vulkan/DX12 头与库）。
#
#   cmake -B build -DZENGINE_BUILD_RHI_REGISTRY_SMOKE_TEST=ON
#   cmake --build build --target RHIFactoryRegistrySmokeTest --config Debug
# =====================================================================
option(ZENGINE_BUILD_RHI_REGISTRY_SMOKE_TEST
       "Build the RHI factory registry smoke-test executable (audit #1 + #4)"
       OFF)

if(ZENGINE_BUILD_RHI_REGISTRY_SMOKE_TEST)
    add_executable(RHIFactoryRegistrySmokeTest RHIFactoryRegistrySmokeTest.cpp)

    set_target_properties(RHIFactoryRegistrySmokeTest PROPERTIES
        CXX_STANDARD 20
        FOLDER "Engine/Tests"
    )

    # ZRuntime 提供 RHIFactory + 各后端注册器 + vulkan-1.lib；PRIVATE 链接避免
    # main() 被 WHOLEARCHIVE 拖进其它消费者。
    target_link_libraries(RHIFactoryRegistrySmokeTest PRIVATE ZRuntime)

    # ZRuntime PRIVATE 链 ZEnginePCH，其 include 目录（EASTL 等）不会传递，需显式补上。
    if(TARGET ZEnginePCH)
        target_link_libraries(RHIFactoryRegistrySmokeTest PRIVATE ZEnginePCH)
        target_include_directories(RHIFactoryRegistrySmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(RHIFactoryRegistrySmokeTest ZEnginePCH)
        target_precompile_headers(RHIFactoryRegistrySmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(RHIFactoryRegistrySmokeTest PRIVATE /W4)
    endif()

    add_test(NAME RHIFactoryRegistrySmokeTest COMMAND RHIFactoryRegistrySmokeTest)

    message(STATUS "Configured RHI factory registry smoke-test target: RHIFactoryRegistrySmokeTest")
endif()
