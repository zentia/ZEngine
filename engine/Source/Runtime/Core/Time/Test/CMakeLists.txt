# =====================================================================
# 固定时间步进（FixedUpdate）冒烟测试
#
#   cmake -B build -DZENGINE_BUILD_FIXED_TIME_STEP_SMOKE_TEST=ON
#   cmake --build build --target FixedTimeStepSmokeTest
#   ctest --test-dir build -C Debug -R FixedTimeStepSmokeTest
#
# 本目录的测试可执行文件自带 main()，必须从 ZRuntime 的 GLOB_RECURSE 里
# carve-out（见 Runtime/CMakeLists.txt 的 REMOVE_ITEM），否则每个消费者 exe
# 都会 LNK2005 "main already defined"。
#
# 为什么这是 FixedUpdate 唯一的守卫
# ---------------------------------
# 固定步进的唯一价值是"模拟推进多少与帧率无关"。这句话成立与否取决于一个
# 累加器的算术，而算错了**没有任何症状**：物理照跑、角色照动、编译照绿，
# 只在"换台机器复现不了 / 联网对不上 / 回放对不上"时显形。
#
# 钉的是：
#   1. 同一段墙钟时间用不同帧率喂进去，总步数一致（特性的定义本身）。
#      用 2 的幂做严格断言，用 30/60/144 fps 做 ±1 的现实组。
#   2. 死亡螺旋闸门（单帧步数上限）与配置夹取（0 步长 => 死循环，必须夹）。
#   3. timeScale / 暂停作用在累加器上，且暂停不攒帧。
#   4. 源码级接线门：LogicalTick 按步数循环、物理已移进 FixedTick。
#      "实现了但没接进主循环 = 没有特性"，而这两种退化都能编译、能运行。
# =====================================================================

option(ZENGINE_BUILD_FIXED_TIME_STEP_SMOKE_TEST
       "Build the fixed-timestep (FixedUpdate) smoke-test executable"
       OFF)

if(ZENGINE_BUILD_FIXED_TIME_STEP_SMOKE_TEST)
    add_executable(FixedTimeStepSmokeTest FixedTimeStepSmokeTest.cpp)

    set_target_properties(FixedTimeStepSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    # 不在这里编任何 .gen.cpp：ZRuntime 的 /WHOLEARCHIVE 已经把全部生成对象
    # 强制拉进本映像（见 ScriptVM/Test/CMakeLists.txt 的踩坑记录）。
    target_link_libraries(FixedTimeStepSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(FixedTimeStepSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(FixedTimeStepSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(FixedTimeStepSmokeTest ZEnginePCH)
        target_precompile_headers(FixedTimeStepSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(APPLE)
        target_sources(FixedTimeStepSmokeTest PRIVATE
            ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
    endif()

    if(MSVC)
        target_compile_options(FixedTimeStepSmokeTest PRIVATE /W4)
    endif()

    # H 组（源码级接线门）需要能读到源文件。
    target_compile_definitions(FixedTimeStepSmokeTest PRIVATE
        "ZENGINE_ENGINE_ROOT_DIR=\"${ENGINE_ROOT_DIR}\"")

    add_test(NAME FixedTimeStepSmokeTest COMMAND FixedTimeStepSmokeTest)
    message(STATUS "Configured fixed-timestep smoke-test target: FixedTimeStepSmokeTest")
endif()
