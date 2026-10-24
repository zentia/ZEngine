#include "Runtime/Core/Memory/UafReporter.h"

#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/Macro.h"

// ============================================================================
// UafReporter 实现
// ----------------------------------------------------------------------------
// 见 UafReporter.h 的判据选型说明：用 instanceID 是否已从 ObjectManager 表注销
// 作为"对象已死"的信号，跨平台一致，不受 Apple 分配器语义分裂影响。
// ============================================================================

namespace zengine::lifetime
{

bool IsInstanceAlive(int32_t instanceID)
{
    // instanceID == 0 表示"无身份"对象（裸赋值 / 尚未分配 ID），不参与校验。
    // 返回 true 即"不触发 UAF"，避免对这类合法但未登记的对象误报。
    if (instanceID == 0)
        return true;

    // ObjectManager 单例未就绪（早期启动 / 静态析构期）时无法判定 → 视为存活，
    // 不在检测的覆盖范围内误伤。运行期单例一定已注册（ObjectManager 是 PreInit）。
    ObjectManager* manager = GET_SYSTEM(ObjectManager);
    if (manager == nullptr)
        return true;

    // IDToPointer 对已注销的 id 返回 nullptr（ObjectManager.cpp:16-26）。
    // 查得到 = 存活；查不到 = 对象已销毁。
    return manager->IDToPointer(instanceID) != nullptr;
}

namespace
{
// function-local static：计数若做成自由全局，会在每个 include 的 TU 各存一份，
// 命中数就散了。这里集中到本 TU，GetUseAfterFreeCount() 读的是同一个计数器。
// 与 MemoryManager.cpp:145-154 的 ForeignFreeCountRef 同一手法。
uint32_t& UafCountRef()
{
    static uint32_t counter = 0;
    return counter;
}
}  // namespace

void ReportUseAfterFree(int32_t instanceID, const char* access_site)
{
    ++UafCountRef();

    // 明确指出"这是 use-after-free，不是普通空指针"：instanceID 已注销意味着
    // 对象析构过、内存可能已归还/复用，继续访问是未定义行为，比 null 解引用更隐蔽。
    LOG_ERROR(ZEngine,
              "use-after-free: access to a destroyed object (instanceID={} at {}). "
              "The object was already destroyed and unregistered; the pointer still "
              "points at the (now-freed) address. This is a dangling-pointer bug, "
              "not a null dereference.",
              instanceID, access_site);

    // Debug/RelWithDebInfo 下当场中断，便于就地定位调用栈。Release 下 ASSERT 为空，
    // 不中断线上运行（报告已在上面落日志）。
    ASSERT(false);
}

uint32_t GetUseAfterFreeCount()
{
    return UafCountRef();
}

}  // namespace zengine::lifetime
