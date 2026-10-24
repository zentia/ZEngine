# =====================================================================
# 本目录的冒烟测试可执行文件自带 main()，因此必须从 ZRuntime 的
# GLOB_RECURSE 里 carve-out（见 Runtime/CMakeLists.txt 的 REMOVE_ITEM），
# 否则每个消费者 exe 都会 LNK2005 "main already defined"。
# =====================================================================

# =====================================================================
# POSIX 崩溃捕获冒烟测试（阶段一·夯地基 第五落点 #2：POSIX 崩溃捕获）
#
#   cmake -B build -DZENGINE_BUILD_POSIX_CRASH_CAPTURE_SMOKE_TEST=ON
#   cmake --build build --target PosixCrashCaptureSmokeTest
#
# 证明非 Windows 的崩溃捕获真的兜得住：fork 子进程装 InstallCrashHandler →
# 解引用空指针触发 SIGSEGV → 父进程校验崩溃产物 crash_stack.txt /
# crash_snap.json 生成、记录到 SIGSEGV、含回溯帧、dladdr 带出模块基址。
#
# 仅 POSIX 上真正运行（fork/signal/backtrace）；Windows/MinGW 上源文件编译成
# 空壳 main()（返回 0，打印 skip 说明），因为该 option 默认 OFF 且 Windows 无
# fork。真实验证留给 Linux/macOS/Android/OHOS 构建——本机（Windows）无 POSIX
# 工具链，无法在此编译运行，故计划文档只标「已实现·待 POSIX 编译验证」。
#
# 需要 -rdynamic / -fno-omit-frame-pointer / -funwind-tables 才能回溯——这些
# 已在顶层 CMakeLists.txt 对非 Windows 全局开启。
# =====================================================================
option(ZENGINE_BUILD_POSIX_CRASH_CAPTURE_SMOKE_TEST
       "Build the POSIX crash-capture smoke-test executable (audit #2)"
       OFF)

if(ZENGINE_BUILD_POSIX_CRASH_CAPTURE_SMOKE_TEST)
    add_executable(PosixCrashCaptureSmokeTest PosixCrashCaptureSmokeTest.cpp)

    set_target_properties(PosixCrashCaptureSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(PosixCrashCaptureSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(PosixCrashCaptureSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(PosixCrashCaptureSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(PosixCrashCaptureSmokeTest ZEnginePCH)
        target_precompile_headers(PosixCrashCaptureSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(PosixCrashCaptureSmokeTest PRIVATE /W4)
    endif()

    add_test(NAME PosixCrashCaptureSmokeTest COMMAND PosixCrashCaptureSmokeTest)

    message(STATUS "Configured POSIX crash-capture smoke test: PosixCrashCaptureSmokeTest")
endif()
