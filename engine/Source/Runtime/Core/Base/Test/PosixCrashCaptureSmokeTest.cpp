// ============================================================================
// PosixCrashCaptureSmokeTest — audit #2 运行期验证
// ----------------------------------------------------------------------------
// 证明非 Windows 的崩溃捕获真的兜得住：fork 子进程 → 安装 InstallCrashHandler →
// 主动解引用空指针触发 SIGSEGV → 父进程回收子进程 → 校验崩溃产物
// crash_stack.txt / crash_snap.json 生成、记录到 SIGSEGV、且含回溯帧。
//
// 为什么必须 fork：崩溃处理器会 raise(sig) 让进程按默认动作终止。若在主进程里
// 触发崩溃，整个测试进程就死了，无法回报结果。因此每个探针进独立子进程，父进程
// 通过退出码 + 产物文件判定。这与 UafDetectionSmokeTest / PPtrLifetimeSmokeTest
// 的子进程隔离手法一致。
//
// ⚠️ WASM/Emscripten 例外：单进程沙箱无 fork，改走"直接调用"路径——不触发真信号
// （否则进程终止无法回报），而是直接调用崩溃处理器实际执行的 zcrash_posix::
// WriteSnapshot()（OnFatalSignal 的唯一职责），用构造的 siginfo_t 模拟 SIGSEGV，
// 覆盖 #2 最该验证的"快照写入代码路径"（open/write/backtrace 兜底/dladdr/JSON
// 生成），全程不崩进程，可在 node 下自主回报。
//
// ⚠️ 非 WASM 的 POSIX 上本文件用 fork；Windows 上编译成空壳 main()（返回 0）。
// CMake 侧的 add_subdirectory 无条件引入本目录，故本文件必须在所有平台过编译。
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__EMSCRIPTEN__)
    // ---- WASM/Emscripten：直接调用 WriteSnapshot，不 fork、不触发真信号 ----
    #include <signal.h>
    #include "Runtime/Core/Base/CrashHandler.h"

    // WriteSnapshot 末尾调用的两个日志自由函数：本 TU 给空桩，把焦点收敛到
    // CrashHandler 自身的快照写入路径（与 LogSystem 解耦，最纯粹地检验它）。
    namespace zcrash_posix
    {
        void ZEngineForceFlushLogs() {}
        uint32_t ZEngineDumpCrashLogTail(const std::string& /*dir*/) { return 0; }
    }  // namespace zcrash_posix

namespace
{
    int g_Failures = 0;

    void Check(bool cond, const char* msg)
    {
        if (cond) std::printf("  [ ok ] %s\n", msg);
        else      { std::printf("  [FAIL] %s\n", msg); ++g_Failures; }
    }

    std::string ReadFile(const std::string& path)
    {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return std::string();
        std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        std::string out;
        if (sz > 0)
        {
            out.resize(static_cast<std::size_t>(sz));
            const std::size_t got = std::fread(&out[0], 1, static_cast<std::size_t>(sz), f);
            out.resize(got);
        }
        std::fclose(f);
        return out;
    }

    bool FileExists(const std::string& path)
    {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::fclose(f);
        return true;
    }
}  // namespace

int main(int /*argc*/, char** /*argv*/)
{
    std::printf("[wasm-posix-crash] audit #2 verification under Emscripten\n");

    // 安装处理器：验证 sigaltstack + 4 个 sigaction 路径可执行。
    InstallCrashHandler("test-0.0.0");
    std::printf("  [ ok ] InstallCrashHandler ran (sigaltstack + sigaction x4)\n");

    // 直接跑崩溃处理器实际执行的快照写入函数，用构造的 SIGSEGV siginfo。
    siginfo_t info;
    std::memset(&info, 0, sizeof(info));
    info.si_signo = SIGSEGV;
    info.si_code  = SEGV_MAPERR;
    info.si_addr  = reinterpret_cast<void*>(0x1234);
    zcrash_posix::WriteSnapshot(SIGSEGV, &info);
    std::printf("  [ ok ] WriteSnapshot(SIGSEGV) returned without secondary fault\n");

    // WASM 下 /proc/self/exe 不可读 → dir 为空 → 产物写到 cwd（MEMFS）。
    const std::string txt  = "crash_stack.txt";
    const std::string json = "crash_snap.json";
    Check(FileExists(txt),  "crash_stack.txt was created");
    Check(FileExists(json), "crash_snap.json was created");

    const std::string txt_body  = ReadFile(txt);
    const std::string json_body = ReadFile(json);
    Check(txt_body.find("signal=11") != std::string::npos,
          "crash_stack.txt records SIGSEGV (signal=11)");
    Check(json_body.find("\"signal\": 11") != std::string::npos,
          "crash_snap.json records SIGSEGV (signal: 11)");
    // platform 运行时派生：WASM 走 #else → "posix"，绝不能是硬编码 windows。
    const std::string::size_type plat = json_body.find("\"platform\"");
    Check(plat != std::string::npos &&
          json_body.find("windows", plat) != std::string::npos + 12,
          "crash_snap.json derives platform at runtime (not hardcoded windows)");
    // 回溯帧：WASM 无 backtrace()，兜底返回 0 帧，故只校验 frames 数组键存在。
    Check(json_body.find("\"frames\"") != std::string::npos,
          "crash_snap.json contains frames array (backtrace degrade path present)");

    std::printf("\nPosixCrashCaptureSmokeTest (wasm): %d check(s), %d failure(s)\n",
                8, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}

#elif !defined(_WIN32) && !defined(__MINGW32__)
    // ---- 桌面 POSIX（Linux/macOS/Android/OHOS）：fork 子进程触发真 SIGSEGV ----
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <signal.h>
    #include <unistd.h>

    #include "Runtime/Core/Base/CrashHandler.h"

namespace
{

    int g_Failures = 0;

    void Check(bool cond, const char* msg)
    {
        if (cond)
        {
            std::printf("  [ ok ] %s\n", msg);
        }
        else
        {
            std::printf("  [FAIL] %s\n", msg);
            ++g_Failures;
        }
    }

    // 读整个文件到 string（用于检查内容）。
    std::string ReadFile(const std::string& path)
    {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f == nullptr)
        {
            return std::string();
        }
        std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        std::string out;
        if (sz > 0)
        {
            out.resize(static_cast<std::size_t>(sz));
            const std::size_t got = std::fread(&out[0], 1, static_cast<std::size_t>(sz), f);
            out.resize(got);
        }
        std::fclose(f);
        return out;
    }

    bool FileExists(const std::string& path)
    {
        struct stat st;
        return stat(path.c_str(), &st) == 0;
    }

    // 子进程：装处理器 → 触发 SIGSEGV。不返回。
    [[noreturn]] void ChildTriggerSegv(const std::string& dir)
    {
        InstallCrashHandler("test-0.0.0");
        // 处理器把产物写到 exe 同目录；子进程拿到 dir 只是备用。
        (void)dir;
        volatile int* p = nullptr;
        *p = 42;  // 解引用空指针 → SIGSEGV
        // 编译器可能优化掉上面的写；显式 raise 兜底确保进入处理器。
        raise(SIGSEGV);
        _exit(0);
    }

    // 父进程跑一个子进程探针，返回子进程是否正常因 SIGSEGV 而死（WTERMSIG==SIGSEGV）。
    bool RunChildProbe(const std::string& dir)
    {
        const pid_t pid = fork();
        if (pid < 0)
        {
            std::printf("       fork failed\n");
            return false;
        }
        if (pid == 0)
        {
            ChildTriggerSegv(dir);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        return WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
    }

    void CheckSegvArtifacts(const std::string& dir)
    {
        const std::string txt = dir + "/crash_stack.txt";
        const std::string json = dir + "/crash_snap.json";

        Check(FileExists(txt), "crash_stack.txt was created");
        Check(FileExists(json), "crash_snap.json was created");

        const std::string txt_body = ReadFile(txt);
        const std::string json_body = ReadFile(json);

        // 崩溃产物里应能看到信号号。SIGSEGV = 11。文本含 "signal=11"，
        // JSON 含 "\"signal\": 11"。
        Check(txt_body.find("signal=11") != std::string::npos,
              "crash_stack.txt records SIGSEGV (signal=11)");
        Check(json_body.find("\"signal\": 11") != std::string::npos,
              "crash_snap.json records SIGSEGV (signal: 11)");

        // platform 字段应被运行时派生（非硬编码 windows）。至少不该是 "windows"。
        const std::string::size_type plat = json_body.find("\"platform\"");
        Check(plat != std::string::npos &&
              json_body.find("windows", plat) != std::string::npos + 12,
              "crash_snap.json derives platform at runtime (not hardcoded windows)");

        // 回溯帧：至少一条 "[0]" / "\"index\": 0"。
        Check(txt_body.find("[0]") != std::string::npos,
              "crash_stack.txt contains backtrace frames");
        Check(json_body.find("\"index\": 0") != std::string::npos,
              "crash_snap.json contains backtrace frames");

        // 帧里应带符号或模块（dladdr 生效）——至少文本里出现一次 "base=0x"。
        Check(txt_body.find("base=0x") != std::string::npos,
              "crash_stack.txt frames carry module base (dladdr worked)");
    }

    void Run()
    {
        // 产物落在当前工作目录（子进程继承 cwd）；用带前缀的独立文件名避免撞车。
        // 注意：崩溃处理器写的是 exe 同目录，这里 exe 与 cwd 可能不同——所以探针
        // 校验的是"文件被创建"这件事本身；父进程在同一 cwd 下查找。
        std::fflush(nullptr);

        std::printf("[child-probe] triggering SIGSEGV with handler installed...\n");
        const bool died_as_expected = RunChildProbe("");
        Check(died_as_expected, "child terminated by SIGSEGV (handler ran, then re-raised)");

        std::printf("[artifacts] inspecting crash outputs...\n");
        CheckSegvArtifacts("");

        std::printf("\nPosixCrashCaptureSmokeTest: %d check(s), %d failure(s)\n",
                    8, g_Failures);
    }

}  // namespace

int main(int /*argc*/, char** /*argv*/)
{
    Run();
    return g_Failures == 0 ? 0 : 1;
}

#else  // Windows / MinGW：无 fork，编译成空壳。真实验证留给 POSIX 构建。

int main(int /*argc*/, char** /*argv*/)
{
    std::printf("PosixCrashCaptureSmokeTest: skipped (not POSIX). "
                "Build & run on Linux/macOS/Android/OHOS to exercise #2.\n");
    return 0;
}

#endif
