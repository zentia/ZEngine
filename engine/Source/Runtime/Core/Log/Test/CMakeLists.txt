# =====================================================================
# 本目录的冒烟测试可执行文件自带 main()，因此必须从 ZRuntime 的
# GLOB_RECURSE 里 carve-out（见 Runtime/CMakeLists.txt 的 REMOVE_ITEM），
# 否则每个消费者 exe 都会 LNK2005 "main already defined"。
# =====================================================================

# =====================================================================
# 崩溃安全日志环形缓冲冒烟测试（阶段一·夯地基 第四落点 #11：日志崩溃环形缓存）
#
#   cmake -B build -DZENGINE_BUILD_CRASH_LOG_TAIL_SMOKE_TEST=ON
#   cmake --build build --target CrashLogTailSmokeTest
#
# 证明 CrashLogTail 的进程级环形缓冲真的能兜住崩溃前的日志尾——
# force_flush_all_logs()（BqLog 异步刷盘，尽力而为）失败时的最后防线：
#   * 单线程：记录 kCapacity 条 → Dump → 全部在、按写入顺序（最旧→最新）。
#   * 溢出回绕：记录 2*kCapacity 条 → 仅保留最近 kCapacity 条、按序。
#   * 并发写不撕裂：N 线程各记 M 条 → 一条不少、每条文本完整（无撕裂）。
#   * 空 dir 是 no-op。
#
# CrashLogTail 刻意零依赖（仅标准头，独立于 BqLog 与 LogSystem 单例），故本测试
# 无需 RegisterCore / 系统 bring-up，也无需 ZENGINE_LIFETIME_CHECK 注入——全程主
# 进程内安全完成，不涉及断言/崩溃，无需子进程隔离。全平台可链可跑。
#
# 注意：本块放在 return()（若有）之前不是必须——本目录 CMakeLists 只此一个目标，
# 但与其它 Test 目录保持一致风格，选项 opt-in、默认 OFF。
# =====================================================================
option(ZENGINE_BUILD_CRASH_LOG_TAIL_SMOKE_TEST
       "Build the crash-safe log-tail ring buffer smoke-test executable"
       OFF)

if(ZENGINE_BUILD_CRASH_LOG_TAIL_SMOKE_TEST)
    add_executable(CrashLogTailSmokeTest CrashLogTailSmokeTest.cpp)

    set_target_properties(CrashLogTailSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(CrashLogTailSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(CrashLogTailSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(CrashLogTailSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(CrashLogTailSmokeTest ZEnginePCH)
        target_precompile_headers(CrashLogTailSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(CrashLogTailSmokeTest PRIVATE /W4)
    endif()

    add_test(NAME CrashLogTailSmokeTest COMMAND CrashLogTailSmokeTest)

    message(STATUS "Configured crash-log tail smoke test: CrashLogTailSmokeTest")
endif()
