#include "ObjectManager.h"

#include "Object.h"
#include "Type.h"

std::vector<std::type_index> ObjectManager::GetDependencies() const
{
    return {};
}

bool ObjectManager::Initialize()
{
    return true;
}

Object* ObjectManager::IDToPointer(int32_t instanceID)
{
    if (instanceID == 0)
        return nullptr;

    std::lock_guard<std::mutex> lock(m_TableMutex);
    auto&& i = m_IDToPointer.find(instanceID);
    if (i != m_IDToPointer.end())
        return i->second;
    return nullptr;
}

Object* ObjectManager::Produce(const Type* type, int32_t instanceID)
{
    Type::FactoryFunction* factory = type->GetFactory();
    if (factory == nullptr)
        return nullptr;
    Object* newObject = factory();
    if (newObject == nullptr)
        return nullptr;

    newObject->m_InstanceID = instanceID;
    newObject->m_CachedTypeIndex = newObject->GetTypeVirtualInternal()->GetRuntimeTypeIndex();

    // Register in the lookup table so PPtr<T>::operator T*() can resolve. Skip
    // when instanceID==0 — that means "deferred id assignment", and registering
    // would collide with whatever zero-id object happens to exist already.
    if (instanceID != 0)
        RegisterInstanceIDInternal(instanceID, newObject);

    return newObject;
}

Object* ObjectManager::AllocateAndAssignInstanceID(Object* obj)
{
    if (obj == nullptr)
        return nullptr;

    if (obj->m_InstanceID == 0)
    {
        // fetch_sub is post-decrement-on-the-counter / post-fetch-on-the-callsite.
        // m_NextHeapInstanceID starts at -1; the first call therefore gets -1 and
        // leaves the counter at -2, the next gets -2 and leaves -3, etc.
        const int32_t newID = m_NextHeapInstanceID.fetch_sub(1, std::memory_order_relaxed);
        obj->m_InstanceID = newID;
    }

    RegisterInstanceIDInternal(obj->m_InstanceID, obj);
    return obj;
}

void ObjectManager::UnregisterInstanceID(int32_t instanceID)
{
    if (instanceID == 0)
        return;

    std::lock_guard<std::mutex> lock(m_TableMutex);
    m_IDToPointer.erase(instanceID);
}

void ObjectManager::RegisterInstanceIDInternal(int32_t instanceID, Object* obj)
{
    std::lock_guard<std::mutex> lock(m_TableMutex);
    m_IDToPointer[instanceID] = obj;
}
