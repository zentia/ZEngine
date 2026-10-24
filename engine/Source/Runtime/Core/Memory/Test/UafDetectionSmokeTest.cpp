// =============================================================================
// UAF 检测冒烟测试（阶段一·夯地基 第一落点 #3 + #5 的运行期验证）
// -----------------------------------------------------------------------------
// 目的：证明 ImmediatePtr 的悬垂校验 + UafReporter **真的能抓到** use-after-free，
// 而不是"编译通过但形同虚设"。规划文档 PHASE1_FOUNDATION_PLAN.md §3.5 里把它列为
// 第一落点的遗留项（只验证了编译 + 宏注入，没验证运行期命中）。本测试补上这一环。
//
// 被测机制回顾（见 ImmediatePtr.h / UafReporter.{h,cpp}）：
//   * ImmediatePtr<T> 反序列化时缓存目标对象的 InstanceID（m_InstanceID）。
//   * operator->() / operator T*() 走 ResolveWithCheck：指针非空、有 id、但该 id
//     已从 ObjectManager 表注销 → 判定 use-after-free → ReportUseAfterFree
//     （计数 + LOG_ERROR + Debug 下 ASSERT(false)）。
//   * ZENGINE_LIFETIME_CHECK：Debug/DebugV8/RelWithDebInfo=1，Release/MinSizeRel=0。
//
// 两条腿：
//   1. 父进程（安全判据，绝不触发真 UAF）：
//      a. IsInstanceAlive 死活翻转 —— 检测机制本身正确。
//      b. round-trip 后的 ImmediatePtr 访问**活**对象 → 不误报（计数仍 0）。
//      c. spawn 自身带 --uaf-probe 子进程，按配置判定其退出码：
//         Debug（ZENGINE_LIFETIME_CHECK=1）期望非零退出（被 ASSERT 中断）；
//         Release（=0）期望零退出（检测被编译掉，行为同改造前）。
//   2. 子进程（--uaf-probe，唯一触发真 UAF 的地方，隔离在独立进程）：
//      round-trip → 访问活对象 OK → 销毁目标对象 → 再访问触发真 UAF。
//      Debug 下 ReportUseAfterFree 的 ASSERT(false) 会 abort，退出码非零；
//      Release 下无断言，正常返回 0。
//
// 为什么要子进程隔离：ReportUseAfterFree 在 Debug 下 ASSERT(false) = 标准 assert()
// → abort()。若在主进程里触发真 UAF，进程会被它自己测的东西杀掉，来不及回传计数、
// 也无法区分"确实抓到了"和"崩溃了"。子进程隔离把"必崩的探针"关进独立进程，父进程
// 只观察退出码，忠实验证端到端，且**不改生产 API**（不为测试削弱断言）。
//
// 为什么用 no-resolver 磁盘 round-trip（P5 手法）：ImmediatePtr 的 m_InstanceID 只能
// 经 Transfer 的反序列化路径回填（私有字段，直接构造是 0、不校验）。no-resolver
// fallback 把 InstanceID 原样塞进 pathID 往返，最贴近生产的内存往返（剪贴板/undo），
// 无需 resolver mock，且读回时 m_InstanceID 被正确回填。
//
// 构建：opt in —— cmake -B build -DZENGINE_BUILD_UAF_DETECTION_SMOKE_TEST=ON
//       cmake --build build --target UafDetectionSmokeTest
// 退出码：0 = 全部判据通过；1 = 有判据失败；77 = 系统 bring-up 失败。
// =============================================================================

#include "RegisterRuntime.h"

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/ObjectDefines.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/ImmediatePtr.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Core/Memory/UafReporter.h"
#include "Runtime/Core/Serialize/SerializationCaching/CachedReader.h"
#include "Runtime/Core/Serialize/SerializationCaching/CachedWriter.h"
#include "Runtime/Core/Serialize/SerializationCaching/FileCacherWrite.h"
#include "Runtime/Core/Serialize/SerializedFile.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

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
// 测试专用 Object 子类。
//   UafTarget —— ImmediatePtr 指向的目标，只带一个 payload 便于识别。
//   UafHolder —— 持有一个 ImmediatePtr<UafTarget>，经 round-trip 后缓存 instanceID。
// 照 PptrSmokeTest.cpp 的 REGISTER_CLASS / DECLARE_OBJECT_SERIALIZE 模式定义。
// =============================================================================
class UafTarget : public Object
{
    REGISTER_CLASS(UafTarget)
    DECLARE_OBJECT_SERIALIZE(UafTarget)

public:
    uint32_t payload {0};
};

class UafHolder : public Object
{
    REGISTER_CLASS(UafHolder)
    DECLARE_OBJECT_SERIALIZE(UafHolder)

public:
    ImmediatePtr<UafTarget> ref;
    int32_t tag {0};
};

IMPLEMENT_REGISTER_CLASS(UafTarget)
IMPLEMENT_OBJECT_SERAILIZE(UafTarget)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(UafTarget)

template<typename TransferFunction>
void UafTarget::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(payload, "payload");
}

IMPLEMENT_REGISTER_CLASS(UafHolder)
IMPLEMENT_OBJECT_SERAILIZE(UafHolder)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(UafHolder)

template<typename TransferFunction>
void UafHolder::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(ref, "ref");
    transfer.Transfer(tag, "tag");
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

constexpr size_t kCacheSize = 4096;

std::filesystem::path ScratchFile(const char* tag)
{
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "zengine_uaf_smoke";
    std::filesystem::create_directories(dir);
    return dir / (std::string("uaf_") + tag + ".zasset");
}

// 造一个带真实 InstanceID 的测试对象（否则 ref 解析不到活对象，检测无从谈起）。
template<typename T>
T* MakeTestObj()
{
    T* obj = MemoryManager::CreateObject<T>();
    GET_SYSTEM(ObjectManager)->AllocateAndAssignInstanceID(obj);
    return obj;
}

// 无 resolver 的磁盘往返（P5 手法）：写 holder（其 ref 指向 target）到盘，读回 holder。
// 读回时 holder.ref 的 m_Ptr 指向**读侧**的 target，m_InstanceID 被回填为读侧 target 的 id。
// 注意：这里 target 不作为独立对象写进文件；ref 的 (fileID=0, pathID=instanceID) 靠
// no-resolver fallback 原样往返（见 ImmediatePtr.cpp 的 ResolveWrite/ResolveRead）。
bool RoundTripHolder(const std::filesystem::path& path, UafHolder* src, UafHolder* dst)
{
    // ---- write ----
    {
        std::filesystem::create_directories(path.parent_path());
        FileCacherWrite cacher;
        if (!cacher.InitWriteFile(path, kCacheSize))
            return false;
        CachedWriter writer;
        writer.InitWrite(cacher);

        SerializedFile* sf = MemoryManager::CreateObject<SerializedFile>();
        sf->InitializeWrite(writer);
        sf->SetAssetGuid(std::string("00000000000000000000000000000000"));
        sf->SetAssetTypeName("UafHolder");

        // 故意不推 ScopedPPtrResolver —— 走 no-resolver fallback，最贴近内存往返。
        sf->WriteObject(*src, /*fileID=*/1);

        FileSize dataOffset {};
        sf->FinishWriting(&dataOffset);
        MemoryManager::DestroyObject(sf);
    }

    // ---- read ----
    {
        SerializedFile* sf = MemoryManager::CreateObject<SerializedFile>();
        if (sf->InitializeRead(path, kCacheSize) != kSerializedFileLoadError_None)
        {
            MemoryManager::DestroyObject(sf);
            return false;
        }
        sf->ReadObject(/*fileID=*/1, *dst);
        MemoryManager::DestroyObject(sf);
    }
    return true;
}

// 触发一次"访问已销毁对象"。这是**唯一**的悬垂解引用点：
//   holder.ref 仍指向 target 的旧地址、缓存着 target 的旧 instanceID；target 已被销毁
//   → id 已从 ObjectManager 注销 → ResolveWithCheck 判定 UAF。
// Debug 下 ReportUseAfterFree 的 ASSERT(false) 会 abort（本函数不会返回）；
// Release 下无断言，读取结果未定义但通常不崩（检测已编译掉，行为同改造前）。
void TriggerDanglingAccess(UafHolder* holder)
{
    // 只解引用一次；Debug 下此行即触发 abort。
    volatile uint32_t sink = holder->ref->payload;
    // Release 下防止 sink 被优化掉，同时不假设读到的值。
    (void)sink;
}

// POSIX fork 分支需要 argv[0]（无 /proc/self/exe 时的可执行路径回退）；
// Windows 用 GetModuleFileName 不需要。main 里保存 argv[0]。
#if !defined(_WIN32)
static char* g_Argv0 = nullptr;
#endif

// spawn 自身带 --uaf-probe 参数，等待结束，回填子进程退出码。返回是否成功 spawn。
bool RunChildProbe(int* exitCode)
{
#if defined(_WIN32)
    char exePath[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return false;
    std::string cmd = std::string("\"") + exePath + "\" --uaf-probe";

    PROCESS_INFORMATION pi {};
    STARTUPINFOA si {};
    si.cb = sizeof(si);
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return false;
    // 超时保护：Debug 下若子进程因断言弹框/死锁挂住，父进程不能跟着永久阻塞。
    // 正常探针（含一次 abort）应在毫秒级退出；30s 是极宽裕的上界。
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
    // POSIX：用 /proc/self/exe 拿可执行路径（Linux）；其它平台退回 argv[0]（main 传入）。
    char exePath[4096] = {0};
    ssize_t n = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (n <= 0)
    {
        // 无 /proc/self/exe（如 macOS）：用父进程传入的 argv[0]。
        if (g_Argv0 != nullptr)
            std::snprintf(exePath, sizeof(exePath), "%s", g_Argv0);
    }
    else
    {
        exePath[n] = '\0';
    }
    if (exePath[0] == '\0')
        return false;  // 无法定位自身可执行文件

    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0)
    {
        // 子进程：execv 自身 + --uaf-probe。
        char* argv[] = {exePath, const_cast<char*>("--uaf-probe"), nullptr};
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
}  // namespace

// =============================================================================
// 子进程探针入口（--uaf-probe）
// =============================================================================
static int RunProbe()
{
#if defined(_WIN32) && defined(_MSC_VER)
    // 关键：MSVC Debug 下 assert(false) 默认弹模态对话框（Abort/Retry/Ignore），
    // 会阻塞子进程使其永不退出、父进程 WaitForSingleObject 卡死。这里把 _CRT_ASSERT
    // 的报告模式改成"只写调试输出、不弹框"——assert 仍会 abort，进程仍以非零码退出，
    // 父进程据此判定"确实抓到了 UAF 并断言"。
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
#endif
    RegisterCore();
    if (!SystemRegistry::GetInstance().InitializeAll(false))
    {
        std::fprintf(stderr, "PROBE FATAL: system bring-up failed\n");
        return 77;
    }

    auto* target = MakeTestObj<UafTarget>();
    target->payload = 0xBEEFCAFE;

    auto* holder = MakeTestObj<UafHolder>();
    holder->ref = target;
    holder->tag = 1;

    const auto path = ScratchFile("probe");
    auto* holder2 = MakeTestObj<UafHolder>();
    if (!RoundTripHolder(path, holder, holder2))
    {
        std::fprintf(stderr, "PROBE FAIL: round-trip failed\n");
        MemoryManager::DestroyObject(target);
        MemoryManager::DestroyObject(holder);
        MemoryManager::DestroyObject(holder2);
        return 1;
    }

    // 读回后 ref 指向读侧 target（holder2 是独立副本，但 ref 解析到同一 target 实例）。
    // 注意：holder2->ref 内部 m_Ptr 指向 target（读侧仍存活），m_InstanceID = target 的 id。
    // 先确认解析成功（非空）。
    if (static_cast<const void*>(holder2->ref) == nullptr)
    {
        std::fprintf(stderr, "PROBE FAIL: ref did not resolve to live target\n");
        MemoryManager::DestroyObject(target);
        MemoryManager::DestroyObject(holder);
        MemoryManager::DestroyObject(holder2);
        return 1;
    }

    // 1) 访问**活**对象：不应触发 UAF。
    TriggerDanglingAccess(holder2);  // target 仍活，Debug 下也不该 abort

    // 2) 销毁目标对象 → ~Object 调 UnregisterInstanceID → id 从表注销。
    //    holder2->ref 此刻起成为悬垂：m_Ptr 指向已释放地址、id 已死。
    MemoryManager::DestroyObject(target);

    // 3) 再访问 → 真 UAF。Debug 下 ASSERT(false) abort（本行不返回，进程非零退出）；
    //    Release 下无断言，继续执行并返回 0。
    TriggerDanglingAccess(holder2);

    MemoryManager::DestroyObject(holder);
    MemoryManager::DestroyObject(holder2);
    return 0;
}

// =============================================================================
// 父进程判据
// =============================================================================
static void CheckParent()
{
    auto* om = GET_SYSTEM(ObjectManager);
    Check(om != nullptr, "ObjectManager is available");

    // ---- (a) IsInstanceAlive 死活翻转：检测机制本身正确 ----
    Check(zengine::lifetime::IsInstanceAlive(0) == true,
          "IsInstanceAlive(0) == true (no-identity objects are not flagged");

    auto* probe = MakeTestObj<UafTarget>();
    const int32_t id = probe->GetInstanceID();
    Check(zengine::lifetime::IsInstanceAlive(id) == true,
          "IsInstanceAlive == true for a live object");
    MemoryManager::DestroyObject(probe);
    Check(zengine::lifetime::IsInstanceAlive(id) == false,
          "IsInstanceAlive == false after Destroy (id unregistered) — the death signal works");

    // ---- (b) round-trip 后访问**活**对象不误报 ----
    const uint32_t countBefore = zengine::lifetime::GetUseAfterFreeCount();

    auto* target = MakeTestObj<UafTarget>();
    target->payload = 0x12345;
    auto* holder = MakeTestObj<UafHolder>();
    holder->ref = target;
    holder->tag = 7;

    const auto path = ScratchFile("parent");
    auto* holder2 = MakeTestObj<UafHolder>();
    Check(RoundTripHolder(path, holder, holder2), "round-trip succeeded");

    // 访问读回的 ref（指向仍存活的 target）—— 必须不误报。
    if (static_cast<const void*>(holder2->ref) != nullptr)
    {
        TriggerDanglingAccess(holder2);  // 活对象，Debug 下也不该 abort
    }
    Check(zengine::lifetime::GetUseAfterFreeCount() == countBefore,
          "accessing a live object after round-trip does NOT false-positive");

    MemoryManager::DestroyObject(target);
    MemoryManager::DestroyObject(holder);
    MemoryManager::DestroyObject(holder2);

    // ---- (c) 子进程探针：Debug 期望抓到并 abort，Release 期望不 abort ----
    int childExit = -1;
    const bool spawned = RunChildProbe(&childExit);
    Check(spawned, "child probe process spawned");

#if ZENGINE_LIFETIME_CHECK
    // Debug/RelWithDebInfo：检测开启，子进程触发真 UAF 应被 ASSERT 中断 → 非零退出。
    Check(spawned && childExit != 0,
          "Debug: child probe aborted on real UAF (detection fires end-to-end)");
    if (!spawned || childExit == 0)
    {
        std::printf("       (child exit code was %d; expected non-zero under ZENGINE_LIFETIME_CHECK=1)\n",
                    childExit);
    }
#else
    // Release/MinSizeRel：检测编译掉，子进程不应因 UAF 断言而 abort → 零退出。
    Check(spawned && childExit == 0,
          "Release: child probe does NOT abort (detection compiled out, zero-overhead)");
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

    // 子进程探针分支：父进程会以 --uaf-probe 拉起自身。
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--uaf-probe") == 0)
            return RunProbe();
    }

    std::fprintf(stderr,
                 "ZEngine UAF detection smoke test (Phase-1, #3 + #5)\n"
                 "===================================================\n");
    std::fflush(stderr);

    RegisterCore();
    if (!SystemRegistry::GetInstance().InitializeAll(false))
    {
        std::fprintf(stderr, "FATAL: SystemRegistry::InitializeAll(false) failed.\n");
        return 77;
    }

    CheckParent();

    std::fprintf(stderr, "\nUAF detection smoke test: %d check(s), %d failure(s)\n",
                 g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
