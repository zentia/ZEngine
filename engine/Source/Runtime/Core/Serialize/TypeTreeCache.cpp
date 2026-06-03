#include "TypeTreeCache.h"

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Serialize/TransferFunctions/GenerateTypeTreeTransfer.h"

bool TypeTreeCache::GetTypeTree(const Object* object, TransferInstructionFlags flags, TypeTree& outTypeTree)
{
    if (object == nullptr)
    {
        outTypeTree = TypeTree();
        return false;
    }
    uint64_t key = object->GetType()->assetTypeID;
    const auto&& iter = m_Cache.find(key);
    if (iter != m_Cache.end())
    {
        outTypeTree = TypeTree(iter->second.data);
        return true;
    }

    outTypeTree = TypeTree();
    Object* nonConstObj = const_cast<Object*>(object);
    GenerateTypeTreeTransfer transfer(outTypeTree, flags, nonConstObj, object->GetType()->size);

    nonConstObj->VirtualRedirectTransfer(transfer);

    return true;
}