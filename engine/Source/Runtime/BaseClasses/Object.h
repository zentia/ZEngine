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

    /// True once the two-phase teardown has started (BeginDestroy -> FinishDestroy
    /// -> delete). See ObjectManager::Destroy / MemoryManager::DestroyObject.
    bool IsDestroying() const { return m_ObjectFlags != ObjectFlags::OF_NoFlags; }

    /// Run the two-phase destruction sequence: set OF_BeginDestroyed, call the
    /// virtual BeginDestroy() hook, set OF_FinishDestroyed, call FinishDestroy().
    /// The memory is NOT freed here -- the caller frees it afterwards (delete or
    /// mimalloc Free depending on how the object was allocated).
    ///
    /// This is the single source of truth that BOTH destruction entry points
    /// (ObjectManager::Destroy for `new`-allocated, MemoryManager::DestroyObject
    /// for mimalloc-allocated) funnel through, so the flags are written no matter
    /// which path an object was created on. Must be called while the object is
    /// still the full most-derived type (before its destructor runs) so a subclass
    /// override of BeginDestroy()/FinishDestroy() dispatches correctly.
    void BeginDestroySequence();

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

    /// The most-derived Type of this instance, via a virtual call.
    ///
    /// Unlike GetType(), this does NOT read m_CachedTypeIndex, so it is valid
    /// even before InitializeRuntimeTypeInfo() has run -- which is exactly the
    /// property dynamic_cast had and Cast<T> (Runtime/BaseClasses/Cast.h) must
    /// keep. The virtual itself stays private: access is checked on the static
    /// type of the expression, so calling it on a `Transform*` would hit
    /// Transform's private override. This non-virtual wrapper runs inside
    /// Object, where the private declaration is visible, and then hands the
    /// result out.
    const Type* GetMostDerivedType() const { return GetTypeVirtualInternal(); }

protected:
    template<typename TransferFunction>
    void Transfer(TransferFunction& transfer);

    /// Current destruction phase flags. Subclasses overriding BeginDestroy()/
    /// FinishDestroy() can inspect this to know how far teardown has progressed.
    ObjectFlags GetFlags() const { return m_ObjectFlags; }

private:
    virtual const Type* GetTypeVirtualInternal() const { return TypeOf<Object>(); }
    ObjectFlags m_ObjectFlags {ObjectFlags::OF_NoFlags};
    uint32_t m_CachedTypeIndex;
    int32_t m_InstanceID;
};

Object* ReadObjectFromPersistentManager(int32_t instanceID);