#pragma once

#include "Runtime/Core/Serialize/TransferFunctions/TransferBase.h"
#include "Runtime/Utility/StringTraits.h"

template<typename TransferFunction>
struct TextTransferTraits;

template<typename Self>
class TextTransferWriteBase : public TransferBase
{
public:
    template<typename T>
    void TransferBase(T& data, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags)
    {
        const char* remappedName = SerializeTraits<T>::GetTypeString(nullptr);
        self().Transfer(data, remappedName, metaFlag);
    }

    template<typename T>
    void TransferStringData(T& data)
    {
        self().TransferStringToCurrentNode(StringTraits::AsConstTChars(data));
    }

protected:
    typedef typename TextTransferTraits<Self>::InternalNodeRef InternalNodeRef;
    typedef typename TextTransferTraits<Self>::MetaParentNodeType MetaParentNodeType;

    struct MetaParent
    {
        MetaParentNodeType node;
        eastl::string name;
    };
    eastl::vector<MetaParent> m_MetaParents;
    eastl::vector<int> m_MetaFlags;
    InternalNodeRef m_CurrentNode;

    TextTransferWriteBase(TransferInstructionFlags flags)
    {
        m_Flags = flags;
        m_CurrentNode = TextTransferTraits<Self>::kInvalidNode;
    }

private:
    Self& self() { return *static_cast<Self*>(this); }
};