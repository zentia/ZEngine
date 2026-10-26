// =============================================================================
// PPtr 生命周期冒烟测试（阶段一·夯地基 第三落点 #6：PPtr 区分"已销毁"与"未解析"）
// -----------------------------------------------------------------------------
// 目的：证明 PPtr<T>::operator T*() 现在能区分 IDToPointer 返回 null 的两种含义，
// 而不是像过去那样无条件掉进 ReadObjectFromPersistentManager 把 UAF 静默掩盖成
// 一次磁盘查询。规划文档 PHASE1_FOUNDATION_PLAN.md #6 的原话：
//   "`PPtr` 把'对象已销毁'静默升级为'从磁盘加载同 ID 对象'（PPtr.h:79），
//    比返回 null 更危险。"
//
// 被测机制回顾（见 PPtr.h operator T*()）：
//   * IDToPointer(id) 返回 null 时，用 **instanceID 的符号** 区分：
//       - 负 ID = 堆对象（ObjectManager 从 -1 起单调递减分配，~Object 注销）
//         → 已销毁仍被解引用 = use-after-free → ReportUseAfterFree + 返回 null，
//           **不再**掉进磁盘 reify。
//       - 正 ID = 磁盘对象（AssetManager remapper 从 2 起单调递增分配）
//         → 尚未 hydrate，惰性加载契约要求从磁盘 reify → 保留 ReadObjectFromPersistentManager。
//   * ZENGINE_LIFETIME_CHECK：Debug/DebugV8/RelWithDebInfo=1，Release/MinSizeRel=0。
//     报告走 UafReporter::ReportUseAfterFree，Debug 下 ASSERT(false) 会 abort。
//
// 两条腿（与 UafDetectionSmokeTest 同手法：子进程隔离必崩探针）：
//   1. 父进程（安全判据，绝不触发真 UAF / 断言）：
//       a. 活堆 PPtr 解析到非空（负 ID 在表里）——正常路径完好。
//       b. 空 PPtr（instanceID==0）解析为 null——无身份对象不解析。
//       c. 销毁后的堆 PPtr 在 **Release** 下解引用返回 null（不返回悬垂地址）——
//          用子进程在 Release 配置下跑，父进程据退出码判定（0=正确）。
//   2. 子进程（--destroyed-probe，唯一触发"销毁堆 PPtr 解引用"处，隔离在独立进程）：
//       构造指向活堆对象的 PPtr → 访问 OK → 销毁目标 → 再解引用。
//       Debug 下 ReportUseAfterFree 的 ASSERT(false) abort → 非零退出；
//       Release 下无断言，operator T*() 返回 null → 返回 0（若竟返回非空则返回 2 = bug）。
//
// 为什么用直接构造（而非磁盘往返）：PPtr(const T*) 走 AssignObject 存下活对象的负 ID，
// 直接、确定性地复现"缓存负 ID 的 PPtr"。序列化往返如何保负 ID 已由 PptrSmokeTest.cpp
// 的 P1-P7 覆盖；本测试只聚焦 #6 的"已销毁 vs 未解析"区分，不需要再走磁盘。
//
// 构建：opt in —— cmake -B build -DZENGINE_BUILD_PPTR_LIFETIME_SMOKE_TEST=ON
//       cmake --build build --target PPtrLifetimeSmokeTest
// 退出码：0 = 全部判据通过；1 = 有判据失败；77 = 系统 bring-up 失败。
// =============================================================================

#include "RegisterRuntime.h"

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/ObjectDefines.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Core/Memory/UafReporter.h"

#include <cstdint>
#include <cstdio>

#if defined(_WIN32)
    #include <windows.h>
    #if defined(_MSC_VER)
        #include <crtdbg.h>  // _CrtSetReportMode：抑制 Debug 断言弹框
    #endif
#else
    #include <sys/wait.h>
    #include <unistd.h>
#endif

// =============================================================================
// 测试专用 Object 子类（照 PptrSmokeTest.cpp 的 REGISTER_CLASS 模式）。
//   LtTarget —— PPtr 指向的目标，带一个 payload 便于识别活对象。
// 只需一个目标类型：本测试用局部 PPtr<LtTarget> 变量，不依赖序列化回填。
// =============================================================================
class LtTarget : public Object
{
    REGISTER_CLASS(LtTarget)
    DECLARE_OBJECT_SERIALIZE(LtTarget)

public:
    uint32_t payload {0};
};

IMPLEMENT_REGISTER_CLASS(LtTarget)
IMPLEMENT_OBJECT_SERAILIZE(LtTarget)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(LtTarget)

template<typename TransferFunction>
void LtTarget::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(payload, "payload");
}

// =============================================================================
// 小工具
// =============================================================================
namespace
{
int g_Checks   = 0;
int g_Failures = 0;

void Check(bool condition, const char* name)
{
    ++g_Checks;
    if (!condition)
    {
        ++g_Failures;
        std::printf("[FAIL] %s\n", name);
    }
    else
    {
        std::printf("[ ok ] %s\n", name);
    }
}

// 造一个带真实（负）InstanceID 的测试对象——否则 PPtr 解析不到活对象，判据无从谈起。
template<typename T>
T* MakeTestObj()
{
    T* obj = MemoryManager::CreateObject<T>();
    GET_SYSTEM(ObjectManager)->AllocateAndAssignInstanceID(obj);
    return obj;
}
}  // namespace

// =============================================================================
// 子进程探针入口（--destroyed-probe）
// =============================================================================
static int RunProbe()
{
#if defined(_WIN32) && defined(_MSC_VER)
    // 关键：MSVC Debug 下 assert(false) 默认弹模态对话框（Abort/Retry/Ignore），
    // 会阻塞子进程使其永不退出、父进程 WaitForSingleObject 卡死。改成只写调试输出、
    // 不弹框——assert 仍会 abort，进程仍以非零码退出，父进程据此判定"确实抓到了"。
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
#endif
    RegisterCore();
    if (!SystemRegistry::GetInstance().InitializeAll(false))
    {
        std::fprintf(stderr, "PROBE FATAL: system bring-up failed\n");
        return 77;
    }

    auto* target = MakeTestObj<LtTarget>();
    target->payload = 0xBEEFCAFE;

    // PPtr 存下 target 的负 instanceID（走 AssignObject）。
    PPtr<LtTarget> pptr(target);

    // 1) 访问**活**对象：IDToPointer(负ID) 命中 → 返回 target。证明正常路径完好。
    LtTarget* live = pptr;
    if (live == nullptr || live->payload != 0xBEEFCAFE)
    {
        std::fprintf(stderr, "PROBE FAIL: live PPtr did not resolve to the live target\n");
        MemoryManager::DestroyObject(target);
        return 1;
    }

    // 2) 销毁目标对象 → ~Object 调 UnregisterInstanceID → 负 ID 从表注销。
    //    pptr 此刻起成为"销毁堆 PPtr"：缓存负 ID、指向已释放地址。
    MemoryManager::DestroyObject(target);

    // 3) 再解引用 → 触发 PPtr operator T*() 的"负 ID 未命中"分支：
    //    Debug 下 ReportUseAfterFree 的 ASSERT(false) abort（本行不返回，非零退出）；
    //    Release 下无断言，operator T*() 返回 null（不再掉进磁盘 reify）。
    LtTarget* destroyed = pptr;

    // Release 下能走到这里：解引用已销毁堆对象必须返回 null（不能返回悬垂地址）。
    if (destroyed != nullptr)
    {
        std::fprintf(stderr, "PROBE FAIL: destroyed-heap PPtr resolved to non-null (dangling address)\n");
        return 2;  // 2 = Release 下的"竟返回悬垂地址"bug 标记
    }

    return 0;
}

// POSIX fork 分支需要 argv[0]（无 /proc/self/exe 时的可执行路径回退）。
// 必须声明在 RunChildProbe 之前：原先放在函数后面，非 Windows 平台根本编不过。
#if !defined(_WIN32)
static char* g_Argv0 = nullptr;
#endif

// spawn 自身带 --destroyed-probe 参数，等待结束，回填子进程退出码。
bool RunChildProbe(int* exitCode)
{
#if defined(_WIN32)
    char exePath[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return false;
    std::string cmd = std::string("\"") + exePath + "\" --destroyed-probe";

    PROCESS_INFORMATION pi {};
    STARTUPINFOA si {};
    si.cb = sizeof(si);
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return false;
    // 超时保护：Debug 下若子进程因断言弹框/死锁挂住，父进程不能跟着永久阻塞。
    const DWORD kTimeoutMs = 30000;
    if (WaitForSingleObject(pi.hProcess, kTimeoutMs) == WAIT_TIMEOUT)
    {
        TerminateProcess(pi.hProcess, 99);  // 99 = 超时强杀标记
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        *exitCode = 99;
        return true;
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    *exitCode = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
#else
    char exePath[4096] = {0};
    ssize_t n = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (n <= 0)
    {
        if (g_Argv0 != nullptr)
            std::snprintf(exePath, sizeof(exePath), "%s", g_Argv0);
    }
    else
    {
        exePath[n] = '\0';
    }
    if (exePath[0] == '\0')
        return false;

    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0)
    {
        char* argv[] = {exePath, const_cast<char*>("--destroyed-probe"), nullptr};
        execv(exePath, argv);
        _exit(127);  // execv 失败
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
        *exitCode = WEXITSTATUS(status);
    else
        *exitCode = -1;  // 被信号杀（SIGABRT 等）
    return true;
#endif
}

// =============================================================================
// 父进程判据
// =============================================================================
static void CheckParent()
{
    auto* om = GET_SYSTEM(ObjectManager);
    Check(om != nullptr, "ObjectManager is available");

    // ---- (a) 活堆 PPtr 解析到非空：正常路径完好（负 ID 在表里）----
    {
        auto* target = MakeTestObj<LtTarget>();
        target->payload = 0x12345;
        PPtr<LtTarget> pptr(target);
        LtTarget* live = pptr;
        Check(live != nullptr && live->payload == 0x12345,
              "live heap PPtr resolves to the live object (negative ID in table)");
        MemoryManager::DestroyObject(target);
    }

    // ---- (b) 空 PPtr（instanceID==0）解析为 null：无身份对象不解析 ----
    {
        PPtr<LtTarget> pptr;  // 默认构造 = 空 PPtr
        Check(pptr == nullptr, "null PPtr (instanceID==0) resolves to null");
    }

    // ---- (c) 子进程探针：销毁堆 PPtr 解引用 ----
    int childExit = -1;
    const bool spawned = RunChildProbe(&childExit);
    Check(spawned, "child probe process spawned");

#if ZENGINE_LIFETIME_CHECK
    // Debug/RelWithDebInfo：检测开启，子进程解引用销毁堆 PPtr 应被 ASSERT 中断 → 非零退出。
    Check(spawned && childExit != 0,
          "Debug: child aborted on destroyed-heap PPtr deref (detection fires end-to-end)");
    if (!spawned || childExit == 0)
    {
        std::printf("       (child exit code was %d; expected non-zero under ZENGINE_LIFETIME_CHECK=1)\n",
                    childExit);
    }
#else
    // Release/MinSizeRel：检测编译掉，子进程解引用返回 null（不返回悬垂地址）→ 零退出。
    Check(spawned && childExit == 0,
          "Release: destroyed-heap PPtr deref returns null (no dangling address, zero-overhead)");
    if (!spawned || childExit != 0)
    {
        std::printf("       (child exit code was %d; expected 0 under ZENGINE_LIFETIME_CHECK=0)\n",
                    childExit);
    }
#endif
}

// =============================================================================
// main
// =============================================================================
int main(int argc, char** argv)
{
#if !defined(_WIN32)
    g_Argv0 = (argc > 0) ? argv[0] : nullptr;
#endif

    // 子进程探针分支：父进程会以 --destroyed-probe 拉起自身。
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--destroyed-probe") == 0)
            return RunProbe();
    }

    std::fprintf(stderr,
                 "ZEngine PPtr lifetime smoke test (Phase-1, #6 destroyed vs unresolved)\n"
                 "=====================================================================\n");
    std::fflush(stderr);

    RegisterCore();
    if (!SystemRegistry::GetInstance().InitializeAll(false))
    {
        std::fprintf(stderr, "FATAL: SystemRegistry::InitializeAll(false) failed.\n");
        return 77;
    }

    CheckParent();

    std::fprintf(stderr, "\nPPtr lifetime smoke test: %d check(s), %d failure(s)\n",
                 g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
