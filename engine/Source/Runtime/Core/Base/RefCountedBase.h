#pragma once

// ============================================================================
// RefCounted<T> — 引用计数混入基类（阶段一·夯地基 第六落点，#7）
// ----------------------------------------------------------------------------
// 目的：给任意引擎对象一个内嵌的强/弱引用计数能力，成为 AtomicRefCounter 的
//   **第二个真实消费者**（此前它只有 SerializedFile 一个消费者，"95% 闲置"）。
//
// 设计取向（关键）：**owner-driven（持有方驱动销毁），不是 shared_ptr 克隆**。
//   * 强计数 Release() 返回"我是不是最后一个强引用"（bool），由持有方决定何时
//     销毁对象——与 SerializedFile::Release() 的既有约定完全一致，也与引擎
//     "对象生命由拥有者负责"的整体风格一致（Object 走 ObjectManager、不走
//     refcount 自动 delete）。
//   * 因此本类**不自动 delete this**，也不提供会 auto-delete 的智能指针。这样
//     彻底规避了"弱指针还要读已被强引用释放掉的控制块"这一 shared_ptr 经典
//     悬垂面（控制块必须在弱引用归零后才释放）。
//   * 弱计数只是"探测对象是否已过期（强计数归零）"的信息，不延长生命、不阻止
//     销毁。对 Object 而言，更地道的弱引用是 WeakObjectPtr（基于 instanceID
//     复用 ObjectManager 的存活探测，见 BaseClasses/WeakObjectPtr.h）——它不
//     依赖本计数，因为 Object 的生命本就由 ObjectManager 驱动。
//
// 用法（CRTP 混入）：
//     class MyAsset : public RefCounted<MyAsset> { ... };
//     MyAsset* a = new MyAsset();
//     a->AddRef();                 // 增加一个强引用（拥有者通常构造即持 1）
//     if (a->Release()) delete a;  // 最后一个强引用 → 拥有者销毁
//     // 弱引用：
//     if (a->AddWeakRef()) { ... a->ReleaseWeakRef(); }  // 登记/撤销弱引用
//     bool gone = a->Expired();    // 强计数是否归零
//
// 线程安全：计数在 AtomicRefCounter 内是 std::atomic，加减多线程安全。销毁决策
//   仍由持有方串行化（同 SerializedFile）。
// ============================================================================

#include "Runtime/Core/Thread/AtomicRefCounter.h"

#include <cstdint>

template<typename Derived>
class RefCounted
{
public:
    RefCounted() = default;
    ~RefCounted() = default;

    RefCounted(const RefCounted&) = delete;
    RefCounted& operator=(const RefCounted&) = delete;

    // ---- 强计数 -----------------------------------------------------------

    /// 增加一个强引用。
    void AddRef() { m_RefCount.Retain(); }

    /// 释放一个强引用，返回"本次是否把强计数减到了 0"（true = 我是最后一个
    /// 强引用，持有方应销毁对象）。与 SerializedFile::Release() 语义一致。
    bool Release() { return m_RefCount.Release(); }

    int32_t StrongRefCount() const { return m_RefCount.StrongCount(); }

    // ---- 弱计数 -----------------------------------------------------------

    /// 登记一个弱引用（不增加强计数、不延长生命）。返回 false 表示对象已过期
    /// （强计数归零），此时登记无意义，调用方应视为"已失效"。
    bool AddWeakRef() { return m_RefCount.AddWeakRef(); }

    /// 撤销一个弱引用。
    void ReleaseWeakRef() { m_RefCount.ReleaseWeakRef(); }

    int32_t WeakRefCount() const { return m_RefCount.WeakCount(); }

    /// 对象是否已过期（强计数归零）。弱引用据此判断目标逻辑上是否已失效。
    bool Expired() const { return m_RefCount.Expired(); }

protected:
    // 内嵌计数器。构造时强计数为 1（"拥有者持有的那一个"），弱计数为 0。
    AtomicRefCounter m_RefCount;
};
