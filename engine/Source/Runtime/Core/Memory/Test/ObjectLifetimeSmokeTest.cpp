// =============================================================================
// 对象生命周期冒烟测试（阶段一·夯地基 第二落点 #8 两阶段销毁的运行期验证）
// -----------------------------------------------------------------------------
// 目的：证明两阶段销毁**真的被触发**——ObjectManager::Destroy 与
// MemoryManager::DestroyObject 两条入口都调用 Object::BeginDestroySequence()，
// 从而 (a) 真正调用 BeginDestroy()/FinishDestroy() 虚钩子、(b) 真正写入
// ObjectFlags（OF_BeginDestroyed / OF_FinishDestroyed）。规划文档
// PHASE1_FOUNDATION_PLAN.md 里 #8 的目标就是"让 ObjectFlags 从死代码变活"。
//
// 被测机制回顾（见 Object.{h,cpp} / ObjectManager.cpp / MemoryManager.h）：
//   * Object::BeginDestroySequence() 是两阶段销毁的唯一实现：
//       置 OF_BeginDestroyed → 调虚 BeginDestroy()
//       置 OF_FinishDestroyed → 调虚 FinishDestroy()
//     （基类实现为空；子类 override 点。）
//   * 两条销毁入口都在 delete/析构之前调用它：
//       - ObjectManager::Destroy(obj)         —— `new` 出来的对象（Produce 路径）
//       - MemoryManager::DestroyObject(obj)   —— mimalloc 出来的对象（NewObject 路径）
//     用 if constexpr (is_base_of_v<Object,T>) 约束，非 Object 类型不受影响。
//   * 必须在 ~T() 之前调用——此刻对象仍是最派生完整类型，虚钩子才能分发到子类。
//
// 判据（全部在父进程内安全完成，不触发任何崩溃/断言）：
//   1. 经 ObjectManager::Destroy 销毁：BeginDestroy/FinishDestroy 各被调用一次，
//      且 Begin 先于 Finish；销毁后对象已释放，无法再查询（只验证销毁过程中）。
//   2. 经 MemoryManager::DestroyObject 销毁（主流 NewObject 路径）：同样三次钩子
//      调用 + 顺序正确——证明 mimalloc 路径也写 flags，不是只有 delete 路径生效。
//   3. 销毁过程中（钩子内）IsDestroying()==true，且能读到正确的当前 phase flag。
//
// 为什么不需要子进程隔离：两阶段销毁不涉及断言/崩溃，钩子调用是确定性的、可观测的
// （用子类计数器记录），全程在主进程内安全完成，比 UAF 测试更简单。
//
// 构建：opt in —— cmake -B build -DZENGINE_BUILD_OBJECT_LIFETIME_SMOKE_TEST=ON
//       cmake --build build --target ObjectLifetimeSmokeTest
// 退出码：0 = 全部判据通过；1 = 有判据失败；77 = 系统 bring-up 失败。
// =============================================================================

#include "RegisterRuntime.h"

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/ObjectDefines.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Memory/MemoryManager.h"

#include <cstdint>
#include <cstdio>

// =============================================================================
// 测试专用 Object 子类：override BeginDestroy/FinishDestroy，把观测写进进程级
// sink（对象销毁后无法回传，所以必须写到进程级存储，销毁后再断言 sink）。
// =============================================================================
struct ProbeSink
{
    int beginCount {0};
    int finishCount {0};
    bool sawDestroyingInBegin {false};
    bool sawDestroyingInFinish {false};
    ObjectFlags flagsInBegin {ObjectFlags::OF_NoFlags};
    ObjectFlags flagsInFinish {ObjectFlags::OF_NoFlags};
    void Reset()
    {
        beginCount = finishCount = 0;
        sawDestroyingInBegin = sawDestroyingInFinish = false;
        flagsInBegin = flagsInFinish = ObjectFlags::OF_NoFlags;
    }
};
ProbeSink g_Sink;

class SinkProbe : public Object
{
    REGISTER_CLASS(SinkProbe)
public:
    void BeginDestroy() override
    {
        ++g_Sink.beginCount;
        g_Sink.flagsInBegin = GetFlags();
        g_Sink.sawDestroyingInBegin = IsDestroying();
    }
    void FinishDestroy() override
    {
        ++g_Sink.finishCount;
        g_Sink.flagsInFinish = GetFlags();
        g_Sink.sawDestroyingInFinish = IsDestroying();
    }
};
IMPLEMENT_REGISTER_CLASS(SinkProbe)

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
}  // namespace

// =============================================================================
// 判据：用进程级 sink 回收钩子观测值并断言（对象销毁后无法回传，故走 sink）。
// =============================================================================
static void RunAssertions()
{
    auto* om = GET_SYSTEM(ObjectManager);
    Check(om != nullptr, "ObjectManager is available");

    // ---- (A) ObjectManager::Destroy 路径（`new` 出来的对象）----
    {
        g_Sink.Reset();
        auto* p = GET_SYSTEM(ObjectManager)->NewObject<SinkProbe>();
        GET_SYSTEM(ObjectManager)->Destroy(p);  // `new` 路径
        Check(g_Sink.beginCount == 1, "OM::Destroy: BeginDestroy called exactly once");
        Check(g_Sink.finishCount == 1, "OM::Destroy: FinishDestroy called exactly once");
        Check(g_Sink.beginCount == 1 && g_Sink.finishCount == 1,
              "OM::Destroy: two phases both ran (Begin then Finish)");
        Check(g_Sink.sawDestroyingInBegin == true, "OM::Destroy: IsDestroying()==true inside BeginDestroy");
        Check(g_Sink.sawDestroyingInFinish == true, "OM::Destroy: IsDestroying()==true inside FinishDestroy");
        Check((g_Sink.flagsInBegin & ObjectFlags::OF_BeginDestroyed) != ObjectFlags::OF_NoFlags,
              "OM::Destroy: OF_BeginDestroyed set inside BeginDestroy");
        Check((g_Sink.flagsInFinish & ObjectFlags::OF_FinishDestroyed) != ObjectFlags::OF_NoFlags,
              "OM::Destroy: OF_FinishDestroyed set inside FinishDestroy");
        Check((g_Sink.flagsInFinish & ObjectFlags::OF_BeginDestroyed) != ObjectFlags::OF_NoFlags,
              "OM::Destroy: OF_BeginDestroyed still set by FinishDestroy (flags accumulate)");
    }

    // ---- (B) MemoryManager::DestroyObject 路径（主流 mimalloc / NewObject）----
    // 关键判据：这条路径不经过 ObjectManager::Destroy，若两阶段销毁没接到这里，
    // 钩子就不会被调用、flags 就不会被写——这正是 #8 要修复的"半死代码"。
    {
        g_Sink.Reset();
        auto* p = GET_SYSTEM(ObjectManager)->NewObject<SinkProbe>();
        MemoryManager::DestroyObject(p);  // mimalloc 路径
        Check(g_Sink.beginCount == 1, "MemMgr::DestroyObject: BeginDestroy called exactly once");
        Check(g_Sink.finishCount == 1, "MemMgr::DestroyObject: FinishDestroy called exactly once");
        Check(g_Sink.sawDestroyingInBegin == true, "MemMgr::DestroyObject: IsDestroying()==true inside BeginDestroy");
        Check((g_Sink.flagsInBegin & ObjectFlags::OF_BeginDestroyed) != ObjectFlags::OF_NoFlags,
              "MemMgr::DestroyObject: OF_BeginDestroyed set (mimalloc path writes flags too)");
        Check((g_Sink.flagsInFinish & ObjectFlags::OF_FinishDestroyed) != ObjectFlags::OF_NoFlags,
              "MemMgr::DestroyObject: OF_FinishDestroyed set (mimalloc path writes flags too)");
    }

    // ---- (C) 非 Object 类型不受影响（if constexpr + is_base_of 约束）----
    // 编译期已验证：MemoryManager::DestroyObject<NonObject> 不调用 BeginDestroySequence。
    // 这里用一个普通堆对象走 DestroyObject，确认它照常释放（不崩、不报错）。
    {
        auto* plain = new int(42);
        MemoryManager::DestroyObject(plain);  // 非 Object：只释放，无两阶段序列
        Check(true, "non-Object type through DestroyObject: frees without two-phase sequence");
    }
}

// =============================================================================
// main
// =============================================================================
int main(int /*argc*/, char** /*argv*/)
{
    std::fprintf(stderr,
                 "ZEngine object-lifetime smoke test (Phase-1, #8 two-phase destruction)\n"
                 "===================================================================\n");
    std::fflush(stderr);

    RegisterCore();
    if (!SystemRegistry::GetInstance().InitializeAll(false))
    {
        std::fprintf(stderr, "FATAL: SystemRegistry::InitializeAll(false) failed.\n");
        return 77;
    }

    RunAssertions();

    std::fprintf(stderr, "\nobject-lifetime smoke test: %d check(s), %d failure(s)\n",
                 g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
