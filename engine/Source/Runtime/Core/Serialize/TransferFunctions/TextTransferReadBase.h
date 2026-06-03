#pragma once

#include "TransferBase.h"

template<typename TransferFunction>
struct TextTransferTraits;

template<typename Self>
class TextTransferReadBase : public TransferBase
{
public:
    // Polymorphic read entry point. IMPLEMENT_OBJECT_SERIALIZE generates
    //   void Concrete::VirtualRedirectTransfer(YAMLRead& t) { t.TransferBase(*this); }
    // for every Object subclass, so `data` here is the concrete type and the
    // call drives that type's Transfer() through SerializeTraits dispatch.
    //
    // This mirrors TextTransferWriteBase::TransferBase: the writer wraps the
    // object's fields under a single key named after the type string, so the
    // reader must navigate into that same key (isRootBase=true selects the
    // type-string key for name conversion). Historically this method was an
    // empty stub -- the JSON read path went through JSONUtility's explicit,
    // statically-typed SerializeTraits call instead of VirtualRedirectTransfer,
    // so polymorphic text deserialization (scenes / prefabs, where the concrete
    // type is only known at runtime) was impossible. Wiring it up enables the
    // YamlObjectGraph multi-object reader.
    template<typename T>
    void TransferBase(T& data, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags)
    {
        const char* remappedName = SerializeTraits<T>::GetTypeString(nullptr);
        self().Transfer(data, remappedName, metaFlag, /*isRootBase=*/true);
    }

protected:
    using InternalNodeRef = typename TextTransferTraits<Self>::InternalNodeRef;

    const char* m_CurrentType;
    InternalNodeRef m_CurrentNode;

    void Init(TransferInstructionFlags flags) {}

    void InitArray() {}

private:
    Self& self() { return *static_cast<Self*>(this); }
};