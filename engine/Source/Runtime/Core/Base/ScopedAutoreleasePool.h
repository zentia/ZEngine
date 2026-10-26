#pragma once

// ScopedAutoreleasePool —— 给纯 C++ 代码用的 autorelease pool（RAII）。
//
// 为什么需要：Apple 上 Metal / AppKit 的大量 API 返回 autorelease 对象
// （`[queue commandBuffer]`、`renderCommandEncoderWithDescriptor:`、
// `[MTLRenderPassDescriptor renderPassDescriptor]` ……）。线程上没有 pool 时，
// runtime 会悄悄建一个**永不 drain** 的隐式 pool，于是这些对象每帧累积、永不释放
// （OBJC_DEBUG_MISSING_POOLS=YES 实测 ZEditor 25 秒内 3000+ 个）。帧循环是 C++
// （Application::TickOneFrame），写不了 `@autoreleasepool`，所以用这个类包一层。
//
// 非 Apple 平台是空操作。必须在**同一个作用域**里构造和析构（NSAutoreleasePool
// 的要求），不能拷贝 / 移动、不能放到堆上跨帧持有。
class ScopedAutoreleasePool
{
public:
#if defined(__APPLE__)
    ScopedAutoreleasePool();
    ~ScopedAutoreleasePool();
#else
    ScopedAutoreleasePool() = default;
    ~ScopedAutoreleasePool() = default;
#endif

    ScopedAutoreleasePool(const ScopedAutoreleasePool&)            = delete;
    ScopedAutoreleasePool& operator=(const ScopedAutoreleasePool&) = delete;
    ScopedAutoreleasePool(ScopedAutoreleasePool&&)                 = delete;
    ScopedAutoreleasePool& operator=(ScopedAutoreleasePool&&)      = delete;

private:
#if defined(__APPLE__)
    void* m_Pool {nullptr};  // NSAutoreleasePool*（头文件保持纯 C++）
#endif
};
