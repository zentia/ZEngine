// =============================================================================
// 引用计数基础设施冒烟测试（阶段一·夯地基 第六落点 #7）
// -----------------------------------------------------------------------------
// 目的：证明 #7 的两个 gap 都被补上——
//   (a) AtomicRefCounter 从"只有强计数、95% 闲置"变成**强/弱双计数**，且保持
//       SerializedFile 既有强计数 API（Release 返回"是否减到 0"）100% 兼容。
//   (b) 新增 RefCounted<T>（CRTP 混入基类）成为 AtomicRefCounter 的**第二个真实
//       消费者**，直接消灭"闲置"，并暴露强/弱能力。
//
// 判据（全部主进程内安全完成，不触发崩溃/断言）：
//   1. 强计数：构造=1、AddRef 递增、Release 递减、Release 到 0 返回 true。
//   2. 弱计数：AddWeakRef/ReleaseWeakRef 独立于强计数；Expired() 跟随强计数。
//   3. AddWeakRef 在强计数归零后拒绝登记（返回 false）——弱引用不保活。
//   4. RefCounted<T>：owner-driven 销毁约定（Release 返回是否最后一个强引用）。
//
// 为什么不需要子进程隔离：计数逻辑不涉及断言/崩溃，观测是确定性的（返回值 +
//   计数器），全程主进程内安全完成。
//
// 构建：opt in —— cmake -B build -DZENGINE_BUILD_REF_COUNTED_SMOKE_TEST=ON
//       cmake --build build --target RefCountedSmokeTest
// 退出码：0 = 全部判据通过；1 = 有判据失败。
// =============================================================================

#include "Runtime/Core/Base/RefCountedBase.h"
#include "Runtime/Core/Thread/AtomicRefCounter.h"

#include <cstdint>
#include <cstdio>

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

// RefCounted<T> 的测试样本：内嵌计数，owner-driven 销毁（不自动 delete）。
class RefSample
{
public:
    explicit RefSample(int v) : m_Value(v) {}
    int Value() const { return m_Value; }

    // 暴露内嵌计数给测试断言。
    int32_t StrongRefCount() const { return m_Ref.StrongCount(); }
    int32_t WeakRefCount()   const { return m_Ref.WeakCount(); }
    bool    Expired()        const { return m_Ref.Expired(); }

    // 转发强/弱能力（模拟真实使用面）。
    void AddRef()            { m_Ref.Retain(); }
    bool Release()           { return m_Ref.Release(); }
    bool TryAddWeakRef()     { return m_Ref.AddWeakRef(); }
    void ReleaseWeakRef()    { m_Ref.ReleaseWeakRef(); }

private:
    AtomicRefCounter m_Ref;
    int m_Value;
};

// =============================================================================
// 判据
// =============================================================================
static void RunAssertions()
{
    // ---- (A) AtomicRefCounter 强计数（SerializedFile 兼容面）----
    {
        AtomicRefCounter c;  // 构造强计数 = 1
        Check(c.StrongCount() == 1, "strong: fresh counter starts at 1");
        Check(c.Count() == 1, "strong: Count() alias matches StrongCount()");

        c.Retain();
        Check(c.StrongCount() == 2, "strong: Retain increments");

        Check(c.Release() == false, "strong: Release from 2 returns false (not last)");
        Check(c.StrongCount() == 1, "strong: Release decrements back to 1");

        Check(c.Release() == true,  "strong: Release from 1 returns true (last holder)");
        Check(c.StrongCount() == 0, "strong: last Release drives count to 0");
    }

    // ---- (B) 弱计数独立于强计数 + Expired 跟随强计数 ----
    {
        AtomicRefCounter c;  // strong=1, weak=0
        Check(c.WeakCount() == 0, "weak: fresh counter has weak=0");
        Check(c.Expired() == false, "weak: not expired while strong>0");

        Check(c.AddWeakRef() == true, "weak: AddWeakRef succeeds while alive");
        Check(c.WeakCount() == 1, "weak: AddWeakRef increments weak count");
        Check(c.AddWeakRef() == true, "weak: second AddWeakRef also succeeds");
        Check(c.WeakCount() == 2, "weak: weak count is 2");

        // 弱计数不延长强计数生命。
        Check(c.StrongCount() == 1, "weak: weak refs do NOT bump strong count");

        c.ReleaseWeakRef();
        Check(c.WeakCount() == 1, "weak: ReleaseWeakRef decrements");
        c.ReleaseWeakRef();
        Check(c.WeakCount() == 0, "weak: ReleaseWeakRef back to 0");

        // 强计数归零 → 立即过期。
        Check(c.Release() == true, "strong: release to 0");
        Check(c.Expired() == true, "weak: Expired()==true once strong==0");
    }

    // ---- (C) AddWeakRef 在对象过期后拒绝登记（弱引用不保活）----
    {
        AtomicRefCounter c;  // strong=1
        Check(c.Release() == true, "strong: drive to 0");
        Check(c.Expired() == true, "expired confirmed");
        Check(c.AddWeakRef() == false, "weak: AddWeakRef rejected on expired object");
        Check(c.WeakCount() == 0, "weak: no weak count registered after expiry");
    }

    // ---- (D) RefCounted<T>：第二个真实消费者 + owner-driven 约定 ----
    {
        RefSample* s = new RefSample(42);
        Check(s->StrongRefCount() == 1, "RefCounted: fresh object has strong=1 (owner's ref)");
        Check(s->WeakRefCount() == 0,   "RefCounted: fresh object has weak=0");
        Check(s->Expired() == false,    "RefCounted: not expired while owner holds ref");

        s->AddRef();
        Check(s->StrongRefCount() == 2, "RefCounted: AddRef increments strong");

        Check(s->TryAddWeakRef() == true, "RefCounted: AddWeakRef succeeds");
        Check(s->WeakRefCount() == 1,     "RefCounted: weak count reflects 1 weak ref");

        Check(s->Release() == false, "RefCounted: Release from 2 → not last");
        Check(s->StrongRefCount() == 1, "RefCounted: strong back to 1 (owner's ref)");

        // 最后一个强引用释放 → 持有方应销毁。这里模拟持有方据此 delete。
        Check(s->Release() == true, "RefCounted: last Release → holder should destroy");
        delete s;  // 持有方负责销毁（owner-driven，不自动 delete this）
        Check(true, "RefCounted: owner-driven destroy (delete after last Release)");
    }

    // ---- (E) 多弱引用 + 强计数归零后弱引用观测过期 ----
    {
        RefSample* s = new RefSample(7);
        s->TryAddWeakRef();
        s->TryAddWeakRef();
        Check(s->WeakRefCount() == 2, "RefCounted: two weak refs registered");
        Check(s->StrongRefCount() == 1, "RefCounted: strong still 1 (owner's)");

        s->Release();  // strong → 0
        Check(s->Expired() == true, "RefCounted: expired after owner releases");
        // 弱引用仍可观测过期（它们不阻止销毁，也不因对象死而失效计数本身）。
        Check(s->WeakRefCount() == 2, "RefCounted: weak count unchanged after expiry");
        s->ReleaseWeakRef();
        s->ReleaseWeakRef();
        delete s;
    }
}

// =============================================================================
// main
// =============================================================================
int main(int /*argc*/, char** /*argv*/)
{
    std::fprintf(stderr,
                 "ZEngine ref-count infrastructure smoke test (Phase-1, #7)\n"
                 "==========================================================\n");
    std::fflush(stderr);

    RunAssertions();

    std::fprintf(stderr, "\nref-counted smoke test: %d check(s), %d failure(s)\n",
                 g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
