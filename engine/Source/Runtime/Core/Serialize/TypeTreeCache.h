#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "SerializationMetaFlags.h"
#include "TypeTree.h"

class Object;

class TypeTreeCache : public IEngineSystem
{
public:
    bool GetTypeTree(const Object* object, TransferInstructionFlags flags, TypeTree& outTypeTree);

protected:
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PreInit; }
    bool Initialize() override { return true; }
    void Shutdown() override {}

private:
    struct CachedTypeTreeData
    {
        bool invalid;
        TransferInstructionFlags flags;
        TypeTreeShareableData* data;
    };
    using TypeTreeCacheCollection = eastl::unordered_map<uint64_t, CachedTypeTreeData>;
    TypeTreeCacheCollection m_Cache;
};