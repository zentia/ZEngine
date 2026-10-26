#pragma once

// ============================================================================
// AtomicRefCounter — 强/弱引用计数（阶段一·夯地基 第六落点，#7）
// ----------------------------------------------------------------------------
// 历史：本类原先继承 std::atomic_int，只有强计数（Retain/Release/Count），全局
//   唯一消费者是 SerializedFile::m_RefCount（用强计数管理一个序列化文件的生死）。
//   引擎里引用计数基础设施"95% 闲置"，且完全没有弱引用能力（见
//   PHASE1_FOUNDATION_PLAN.md #7）。本落点在**保持 SerializedFile 强计数 API
//   100% 兼容**（Release 仍返回"是否减到 0"）的前提下补上弱计数，并新增
//   RefCounted<T>（Core/Base/RefCountedBase.h）作为第二个真实消费者，直接消灭
//   "闲置"。
//
// 强/弱计数语义（同 std::shared_ptr 的控制块模型，但内嵌在计数对象里，避免手写
//   易错的控制块分离）：
//   * 强计数（strong）：Retain +1 / Release -1。Release 返回"本次是否把强计数减
//     到了 0"（即"我是不是最后一个强引用"），由持有方决定销毁对象——与
//     SerializedFile::Release() 的既有语义完全一致。
//   * 弱计数（weak）：AddWeakRef +1 / ReleaseWeakRef -1。弱引用不增加强计数、
//     不延长对象生命，只用于探测"对象是否已过期（强计数归零）"。
//   * Expired()：强计数是否已归零。弱引用用它判断对象逻辑上是否已失效。
//
// 线程安全：强、弱计数各自是 std::atomic，加减用默认的 seq_cst 序，多线程安全。
// ============================================================================

#include <atomic>
#include <cstdint>

class AtomicRefCounter
{
public:
    AtomicRefCounter()
        : m_Strong(1), m_Weak(0) {}

    // ---- 强计数（SerializedFile 既有消费者依赖这三个）---------------------

    void Retain() { m_Strong.fetch_add(1); }

    /// 强计数 -1，返回"本次是否把强计数减到了 0"（true = 我是最后一个强引用，
    /// 持有方应销毁对象）。与 SerializedFile::Release() 的既有返回语义一致。
    bool Release()
    {
        const int32_t beforeDecrement = m_Strong.fetch_sub(1);
        return beforeDecrement == 1;
    }

    int StrongCount() const { return m_Strong.load(std::memory_order_relaxed); }

    // 兼容性别名：改造前叫 Count()，保留以免任何外部调用点断裂。
    int Count() const { return StrongCount(); }

    // ---- 弱计数（#7 新增）-------------------------------------------------

    /// 登记一个弱引用。弱引用不增加强计数、不延长对象生命。返回 false 表示对象
    /// 已经过期（强计数归零），此时登记无意义——调用方应视为"已失效"。
    bool AddWeakRef()
    {
        // 只有强计数 > 0（对象还活着）时才登记弱引用。用 CAS 循环避免与 Release
        // 竞态：读取到 0 则拒绝；若读取后期间被减到 0 则回退。
        int32_t expected = m_Strong.load(std::memory_order_relaxed);
        while (expected > 0)
        {
            if (m_Strong.compare_exchange_weak(expected, expected,
                                               std::memory_order_relaxed))
                break;  // 强计数未变，继续走下面的弱计数 +1
            if (expected == 0)
                return false;
        }
        if (expected == 0)
            return false;
        m_Weak.fetch_add(1);
        return true;
    }

    /// 撤销一个弱引用。弱计数归零且强计数归零时，意味着再无强/弱引用，可安全
    /// 释放控制资源（本类不持有需释放的资源，故这里只做计数回退）。
    void ReleaseWeakRef() { m_Weak.fetch_sub(1); }

    int WeakCount() const { return m_Weak.load(std::memory_order_relaxed); }

    /// 对象是否已过期（强计数归零）。弱引用据此判断目标逻辑上是否已失效。
    bool Expired() const { return m_Strong.load(std::memory_order_relaxed) == 0; }

private:
    std::atomic<int32_t> m_Strong;
    std::atomic<int32_t> m_Weak;
};
