#include "Runtime/Core/Base/ScopedAutoreleasePool.h"

#import <Foundation/Foundation.h>

// 仓库是 MRR（没有 -fobjc-arc），所以可以直接用 NSAutoreleasePool —— 它就是
// `@autoreleasepool` 在 MRR 下的等价物（push 在 init、pop 在 drain）。
ScopedAutoreleasePool::ScopedAutoreleasePool() : m_Pool([[NSAutoreleasePool alloc] init]) {}

ScopedAutoreleasePool::~ScopedAutoreleasePool()
{
    [(NSAutoreleasePool*)m_Pool drain];
    m_Pool = nullptr;
}
