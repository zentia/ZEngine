// =============================================================================
// 弱引用句柄冒烟测试（阶段一·夯地基 第六落点 #7）
// -----------------------------------------------------------------------------
// 目的：证明新增的 WeakObjectPtr<T>（基于 instanceID 的弱句柄）行为正确——
//   * 对象存活时解析为活指针；
//   * 对象销毁后（instanceID 从 ObjectManager 表注销）弱引用**优雅退化为 null**，
//     且**不报告 UAF**（这是弱引用与 ImmediatePtr 强引用的关键区别：访问已死
//     对象对弱引用是合法语义，不该报 use-after-free）。
//
// 判据（全部主进程内安全完成，不触发崩溃/断言）：
//   1. 活对象：WeakObjectPtr 解析回同一活指针、operator bool 为真、非 null。
//   2. 销毁后：弱引用 Get()==nullptr、IsNull 语义正确、IsExpired()==true。
//   3. 对比强引用判据：弱引用访问死对象**不**增加 UAF 计数（zengine::lifetime
//      ::GetUseAfterFreeCount() 前后不变）——证明它走存活探测、不走 UAF 报告。
//   4. 无身份对象（id==0）：弱句柄为空、不过期、解析为 null。
//
// 为什么不需要子进程隔离：弱引用解析不涉及断言/崩溃（访问死对象合法返回 null），
//   全程主进程内安全完成。UAF 计数前后对比即可证明"不误报"。
//
// 构建：opt in —— cmake -B build -DZENGINE_BUILD_WEAK_OBJECT_PTR_SMOKE_TEST=ON
//       cmake --build build --target WeakObjectPtrSmokeTest
// 退出码：0 = 全部判据通过；1 = 有判据失败；77 = 系统 bring-up 失败。
// =============================================================================

#include "RegisterRuntime.h"

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/BaseClasses/WeakObjectPtr.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Memory/UafReporter.h"

#include <cstdint>
#include <cstdio>

// 测试专用 Object 子类（需要 REGISTER_CLASS 才能经 ObjectManager::NewObject 创建）。
class WeakProbe : public Object
{
    REGISTER_CLASS(WeakProbe)
public:
    int Value {0};
    WeakProbe() { Value = 1234; }
};
IMPLEMENT_REGISTER_CLASS(WeakProbe)

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

static void RunAssertions()
{
    auto* om = GET_SYSTEM(ObjectManager);
    Check(om != nullptr, "ObjectManager is available");

    // ---- (A) 活对象：弱句柄解析回活指针 ----
    WeakObjectPtr<WeakProbe> wp;
    {
        auto* p = om->NewObject<WeakProbe>();
        Check(p != nullptr, "NewObject returned a live object");
        Check(p->GetInstanceID() != 0, "live object has a non-zero instanceID");

        wp = p;  // 从活对象构造弱句柄
        Check(wp.GetInstanceID() == p->GetInstanceID(), "weak ptr captured the object's instanceID");
        Check(wp.Get() == p, "weak ptr resolves to the SAME live pointer");
        Check((bool)wp == true, "weak ptr is truthy while object alive");
        Check(wp.IsNull() == false, "weak ptr is not null while object alive");
        Check(wp.IsExpired() == false, "weak ptr is not expired while object alive");
        Check(wp->Value == 1234, "weak ptr operator-> reaches live member");

        // ---- (B) 销毁后：弱句柄优雅退化为 null，且不报 UAF ----
        const uint32_t uafBefore = zengine::lifetime::GetUseAfterFreeCount();
        om->Destroy(p);  // 对象销毁，instanceID 从表注销

        Check(wp.Get() == nullptr, "weak ptr resolves to null after object destroyed");
        Check((bool)wp == false, "weak ptr is falsy after object destroyed");
        Check(wp.IsExpired() == true, "weak ptr reports expired after object destroyed");
        Check(wp.IsNull() == false, "weak ptr still holds its instanceID (not null id) after destroy");

        // 关键判据：弱引用访问死对象**不**报告 UAF（与 ImmediatePtr 强引用相反）。
        const uint32_t uafAfter = zengine::lifetime::GetUseAfterFreeCount();
        Check(uafAfter == uafBefore,
              "weak ptr access to destroyed object does NOT report UAF (legal weak semantics)");
    }

    // ---- (C) 无身份对象（id==0）：空弱句柄 ----
    {
        WeakObjectPtr<WeakProbe> empty;  // 默认构造，id==0
        Check(empty.IsNull() == true, "default weak ptr is null (no identity)");
        Check((bool)empty == false, "default weak ptr is falsy");
        Check(empty.IsExpired() == false, "null-id weak ptr is not 'expired' (it was never valid)");
        Check(empty.Get() == nullptr, "null-id weak ptr resolves to null");
    }

    // ---- (D) 从 instanceID 构造的弱句柄：活对象可解析 ----
    {
        auto* p = om->NewObject<WeakProbe>();
        const int32_t id = p->GetInstanceID();
        WeakObjectPtr<WeakProbe> fromId(id);
        Check(fromId.Get() == p, "weak ptr built from instanceID resolves to live object");
        om->Destroy(p);
        Check(fromId.Get() == nullptr, "weak ptr from instanceID resolves null after destroy");
        Check(fromId.IsExpired() == true, "weak ptr from instanceID reports expired after destroy");
    }
}

// =============================================================================
// main
// =============================================================================
int main(int /*argc*/, char** /*argv*/)
{
    std::fprintf(stderr,
                 "ZEngine weak-object-pointer smoke test (Phase-1, #7)\n"
                 "===================================================\n");
    std::fflush(stderr);

    RegisterCore();
    if (!SystemRegistry::GetInstance().InitializeAll(false))
    {
        std::fprintf(stderr, "FATAL: SystemRegistry::InitializeAll(false) failed.\n");
        return 77;
    }

    RunAssertions();

    std::fprintf(stderr, "\nweak-object-pointer smoke test: %d check(s), %d failure(s)\n",
                 g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
