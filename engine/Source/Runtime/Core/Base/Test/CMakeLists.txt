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
# fork。真实验证留给 Linux/macOS/Android/OHOS 构建。
#
# WASM/Emscripten 例外：单进程沙箱无 fork，源文件的 __EMSCRIPTEN__ 分支改走
# "直接调用" —— 不触发真信号，而是直接跑崩溃处理器实际执行的
# zcrash_posix::WriteSnapshot()（OnFatalSignal 的唯一职责），覆盖快照写入代码
# 路径（open/write/backtrace 兜底/dladdr/JSON 生成）。该分支与 LogSystem 解耦
# （本 TU 内给两个日志自由函数空桩），可脱离整个 ZRuntime 独立编译验证：
#   source /c/emsdk/emsdk_env.sh
#   emcc PosixCrashCaptureSmokeTest.cpp -std=c++23 -I<engine>/Source \
#       -o t.js -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1 && \
#   node -e "require('./t.js');setTimeout(()=>process.exit(0),3000)"
# 该验证已在 Emscripten 3.1.74 下跑通（8/8），并借此揪出并修复了 WriteDec/
# WriteHex 的 NUL 落盘 + 0x 双前缀两个跨平台真 bug（见 CrashHandler.h）。
# 本机（Windows）桌面 POSIX 工具链缺失，故 Linux 运行期确认仍待真机构建。
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
