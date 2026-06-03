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
            manager->UnregisterInstanceID(m_InstanceID);
    }
}

void Object::BeginDestroy() {}
void Object::FinishDestroy() {}

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