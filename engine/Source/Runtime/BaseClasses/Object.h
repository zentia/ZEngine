#pragma once

#include "ObjectDefines.h"
#include "Runtime/Core/Base/Macro.h"
#include "Type.h"
#include "TypeManager.h"
#include "Utility/NonCopyable.h"

#include <string>
class JSONRead;
class JSONWrite;
class YAMLRead;
class YAMLWrite;
class StreamedBinaryRead;
class StreamedBinaryWrite;
class GenerateTypeTreeTransfer;
class SafeBinaryRead;

class Object : public NonCopyable
{
    friend class ObjectManager;

public:
    struct kTypeFlags
    {
        enum
        {
            value = kTypeIsAbstract
        };
    };
    using ThisType = Object;

    /// Virtual so that `MemoryManager::DestroyObject<Base>(derived_ptr)` correctly
    /// invokes the most-derived destructor. The destructor body lives in Object.cpp
    /// because it needs ObjectManager to deregister the instance ID — and we want
    /// to keep ObjectManager out of this header to avoid a circular include with
    /// PPtr.h (PPtr.h #includes ObjectManager.h, ObjectManager.h needs to know
    /// Object exists).
    virtual ~Object();

    virtual void BeginDestroy();
    virtual void FinishDestroy();
    eastl::string name;

    bool Is(const Type* type) const { return type->IsBaseOf(m_CachedTypeIndex); }
    bool Initialize() { return true; }
    int32_t GetInstanceID() const { return m_InstanceID; }

    // PPtr<Object> support: PPtr<T>::GetTypeString() expands to T::GetPPtrTypeString().
    // REGISTER_CLASS injects this on every concrete subclass; Object itself isn't
    // registered through that macro path (it's bootstrapped by Type.h directly), so
    // we hand-author the equivalent here. Without it, any field of type
    // PPtr<Object> — which the editor-side Prefab system needs for the
    // type-erased PropertyModification.target — would fail to compile inside
    // SerializeTraits<PPtr<T>>::GetTypeString.
    static const char* GetPPtrTypeString() { return "PPtr<Object>"; }

    virtual void VirtualRedirectTransfer(JSONRead&)
    {
        LOG_FATAL(ZEngine, "Serialization not implemented for type {}", GetTypeName());
    }
    virtual void VirtualRedirectTransfer(JSONWrite&) {}
    virtual void VirtualRedirectTransfer(YAMLRead&)
    {
        LOG_FATAL(ZEngine, "Serialization not implemented for type {}", GetTypeName());
    }
    virtual void VirtualRedirectTransfer(YAMLWrite&) {}
    virtual void VirtualRedirectTransfer(StreamedBinaryRead&) {}
    virtual void VirtualRedirectTransfer(StreamedBinaryWrite&) {}
    virtual void VirtualRedirectTransfer(GenerateTypeTreeTransfer&) {}
    virtual void VirtualRedirectTransfer(SafeBinaryRead&) {}
    const Type* GetType() const { return TypeManager::GetInstance().GetRuntimeTypes().types[m_CachedTypeIndex]; }
    const char* GetTypeName() const { return GetType()->GetName(); }
    void InitializeRuntimeTypeInfo();

protected:
    template<typename TransferFunction>
    void Transfer(TransferFunction& transfer);

private:
    virtual const Type* GetTypeVirtualInternal() const { return TypeOf<Object>(); }
    ObjectFlags m_ObjectFlags {ObjectFlags::OF_NoFlags};
    uint32_t m_CachedTypeIndex;
    int32_t m_InstanceID;
};

Object* ReadObjectFromPersistentManager(int32_t instanceID);