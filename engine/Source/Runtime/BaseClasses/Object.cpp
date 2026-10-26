#include "Object.h"

#include "ObjectManager.h"
#include "Runtime/Resource/Asset/AssetManager.h"

Object::~Object()
{
    // Drop our entry from the global IDToPointer table so future PPtr resolves of
    // the same id correctly return nullptr (or whatever asset-loader replaces us
    // with). Safe at static-shutdown because UnregisterInstanceID(0) is a no-op
    // and uninitialized objects keep m_InstanceID==0 until AllocateAndAssignInstanceID.
    if (m_InstanceID != 0)
    {
        if (auto&& manager = GET_SYSTEM(ObjectManager))
            manager->UnregisterInstanceID(m_InstanceID, this);
    }
}

void Object::BeginDestroy() {}
void Object::FinishDestroy() {}

void Object::BeginDestroySequence()
{
    // Two-phase teardown, driven from here so BOTH destruction entry points
    // (ObjectManager::Destroy for `new`-allocated objects, and
    // MemoryManager::DestroyObject for mimalloc-allocated ones) funnel through a
    // single implementation -- otherwise the flags would stay unwritten on the
    // mimalloc path (the common case for NewObject), making ObjectFlags half-dead
    // code exactly as it was before Phase-1 #8.
    //
    // Called while this is still the full most-derived type (the callers invoke
    // this *before* delete / before ~T() runs), so a subclass override of
    // BeginDestroy()/FinishDestroy() dispatches to the most-derived version.
    m_ObjectFlags |= ObjectFlags::OF_BeginDestroyed;
    BeginDestroy();

    m_ObjectFlags |= ObjectFlags::OF_FinishDestroyed;
    FinishDestroy();
}

void Object::InitializeRuntimeTypeInfo()
{
    m_InstanceID = 0;

    const Type* runtime_type = GetTypeVirtualInternal();
    m_CachedTypeIndex = (runtime_type != nullptr) ? runtime_type->GetRuntimeTypeIndex() : Type::DefaultTypeIndex;
}

Object* ReadObjectFromPersistentManager(int32_t instanceID)

{
    return GET_SYSTEM(AssetManager)->ReadObject(instanceID);
}

INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(Object, EXPORTDLL, Transfer, void);

template<typename TransferFunction>
void Object::Transfer(TransferFunction& transfer)
{
}