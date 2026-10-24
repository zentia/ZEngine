#pragma once

// ============================================================================
// WeakObjectPtr<T> — 弱引用句柄（阶段一·夯地基 第六落点，#7）
// ----------------------------------------------------------------------------
// 背景：引擎的句柄家族原先只有"强引用/持久引用"成员——PPtr<T>（磁盘序列化引用，
//   惰性从磁盘 reify）、ImmediatePtr<T>（裸指针 + UAF 校验的强引用）。**独独没有
//   弱引用**（见 PHASE1_FOUNDATION_PLAN.md #7）。本类补齐这一成员。
//
// 语义：基于 Object 的 instanceID 的**弱句柄**。它不拥有对象、不延长对象生命，
//   只在访问时探测对象是否仍存活：
//     * 对象存活 → 返回活指针。
//     * 对象已销毁（instanceID 已从 ObjectManager 表注销）→ 返回 nullptr。
//
// 与 ImmediatePtr 的关键区别（务必分清）：
//   * ImmediatePtr 是**强引用**：访问一个已销毁对象会**报告 UAF**（这是"你握着
//     强引用却访问了死对象"的错误）。
//   * WeakObjectPtr 是**弱引用**：访问一个已销毁对象**合法地返回 null、不报告
//     UAF**。弱引用的存在意义就是"允许目标先死、我优雅地退化为 null"。因此
//     本类复用 IsInstanceAlive 的**存活探测**，但刻意不走 ReportUseAfterFree。
//
// 复用而非重造：存活判据完全复用 zengine::lifetime::IsInstanceAlive（即
//   ObjectManager::IDToPointer 查表），与 PPtr/ImmediatePtr 的判据同源，跨平台
//   一致，不受 Apple 分配器语义分裂影响。
//
// 纯运行期句柄：**不参与序列化**（Transfer）。弱引用本质是运行期关系——它不
//   拥有目标、目标生死由别处决定，序列化一个"不保活"的引用没有稳定语义。需要
//   持久化引用请用 PPtr<T>。
//
// 线程安全：instanceID 读写无锁；解析走 ObjectManager（其表有 m_TableMutex）。
// ============================================================================

#include "Runtime/Core/Memory/UafReporter.h"

#include <stdint.h>

class Object;

// 解析 instanceID → 活 Object*。定义在 WeakObjectPtr.cpp（保持本 header 轻量，
// 不拖入 ObjectManager / Object 的完整定义）。前置声明供 Resolve() 调用。
Object* WeakObjectPtrResolveRaw(int32_t instanceID);

template<typename T>
class WeakObjectPtr
{
public:
    WeakObjectPtr()
        : m_InstanceID(0) {}

    /// 从活对象构造弱句柄。捕获其 instanceID；id==0（无身份对象）退化为空。
    explicit WeakObjectPtr(const T* obj)
        : m_InstanceID(obj != nullptr ? obj->GetInstanceID() : 0) {}

    /// 从 instanceID 构造（例如先前解析到的 id）。
    explicit WeakObjectPtr(int32_t instanceID)
        : m_InstanceID(instanceID) {}

    /// 赋值：重新绑定到另一个活对象（同 ImmediatePtr 的裸赋值语义，但保留弱句柄
    /// 身份——捕获新目标的 instanceID）。
    WeakObjectPtr& operator=(const T* obj)
    {
        m_InstanceID = obj != nullptr ? obj->GetInstanceID() : 0;
        return *this;
    }

    /// 赋值：重新绑定到另一个弱句柄。
    WeakObjectPtr& operator=(const WeakObjectPtr& other)
    {
        m_InstanceID = other.m_InstanceID;
        return *this;
    }

    int32_t GetInstanceID() const { return m_InstanceID; }

    /// 解析为活对象指针。对象已销毁 / 无身份 → 返回 nullptr（**不报告 UAF**，
    /// 这是弱引用的合法语义）。
    T* Get() const { return Resolve("WeakObjectPtr<T>::Get"); }

    /// 便捷解引用（返回活指针或 null）。
    operator T*() const { return Get(); }

    T* operator->() const { return Get(); }

    /// 显式布尔：弱句柄当前是否指向一个存活对象。
    explicit operator bool() const { return Resolve("WeakObjectPtr<T>::operator bool") != nullptr; }

    bool IsNull() const { return m_InstanceID == 0; }

    /// 对象是否已失效（弱引用意义上的"过期"）：有身份、但已不在 ObjectManager
    /// 表里。无身份（id==0）不算过期（它本就空）。
    bool IsExpired() const
    {
        if (m_InstanceID == 0)
            return false;
        return !zengine::lifetime::IsInstanceAlive(m_InstanceID);
    }

private:
    // 集中一处解析，operator T*() / operator->() / Get() / operator bool 复用。
    // 弱引用访问已销毁对象是合法的（返回 null），故只做存活探测、不报 UAF。
    T* Resolve(const char* /*access_site*/) const
    {
        if (m_InstanceID == 0)
            return nullptr;
        // IsInstanceAlive 内部走 ObjectManager::IDToPointer：查得到 = 存活，
        // 查不到 = 已销毁。弱引用对两种结果都不报错。
        if (!zengine::lifetime::IsInstanceAlive(m_InstanceID))
            return nullptr;
        // 活对象：instanceID 此刻一定在表里，解析回活指针。Object* → T* 由
        // 调用方静态类型完成（WeakProbe 等派生类型经 REGISTER_CLASS 与 Object
        // 是标准多态关系，static_cast 安全）。
        return static_cast<T*>(WeakObjectPtrResolveRaw(m_InstanceID));
    }

    // 解析 instanceID → 活 Object*。定义在 WeakObjectPtr.cpp（保持本 header 轻量，
    // 不拖入 ObjectManager / Object 的完整定义）。
    friend Object* WeakObjectPtrResolveRaw(int32_t instanceID);

    int32_t m_InstanceID;
};
