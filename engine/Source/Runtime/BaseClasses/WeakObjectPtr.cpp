#include "Runtime/BaseClasses/WeakObjectPtr.h"

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/Macro.h"

// WeakObjectPtrResolveRaw — WeakObjectPtr<T>::Resolve 的活对象解析实现。
// 定义在 .cpp 里，使 WeakObjectPtr.h 保持轻量（不拖入 ObjectManager / Object 的
// 完整定义，同 ImmediatePtr.cpp 的手法）。返回 instanceID 对应的活 Object*，
// 查不到（已销毁 / 无身份）返回 nullptr。
Object* WeakObjectPtrResolveRaw(int32_t instanceID)
{
    if (instanceID == 0)
        return nullptr;

    ObjectManager* manager = GET_SYSTEM(ObjectManager);
    if (manager == nullptr)
        return nullptr;  // 单例未就绪（早期启动 / 静态析构期）→ 无法解析

    return manager->IDToPointer(instanceID);
}
