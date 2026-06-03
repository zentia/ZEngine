#include "GenerateTypeTreeTransfer.h"

GenerateTypeTreeTransfer::GenerateTypeTreeTransfer(TypeTree& t, TransferInstructionFlags flags, void* objectPtr, int objectSize)
    : m_TypeTree(t)
{
    m_Flags = flags;

    m_ObjectPtr = reinterpret_cast<char*>(objectPtr);
    m_ObjectSize = objectSize;
    m_Index = 0;
}

void GenerateTypeTreeTransfer::BeginTransfer(const char* name, const char* typeString, char* data, TransferMetaFlags metaFlag)
{
    TypeTreeIterator typeItr;
    TypeTreeNode* typeTreeNode;

    if (!m_ActiveFather.IsNull())
    {
        typeItr = m_ActiveFather.AddChildNode(m_TypeTree);
        typeTreeNode = typeItr.GetWritableNode(m_TypeTree);
        typeTreeNode->m_MetaFlag = metaFlag | m_ActiveFather->m_MetaFlag;
        typeTreeNode->m_MetaFlag &= ~(TransferMetaFlags::kAnyChildUsesAlignBytesFlag);
    }
    else
    {
        typeItr = m_TypeTree.Root();
        typeTreeNode = typeItr.GetWritableNode(m_TypeTree);
        typeTreeNode->m_MetaFlag = metaFlag;
    }

    m_TypeTree.AssignTypeString(typeItr, typeString);
    m_TypeTree.AssignNameString(typeItr, name);
    typeTreeNode->m_ByteSize = 0;

    typeTreeNode->m_Index = m_Index++;
    if (data != nullptr)
    {
        if (m_ObjectPtr != nullptr)
        {
            constexpr ptrdiff_t kFallbackInspectableSpan = 16 * 1024;
            const ptrdiff_t offset = data - m_ObjectPtr;
            const ptrdiff_t valid_span = m_ObjectSize > 0 ? std::max<ptrdiff_t>(m_ObjectSize, kFallbackInspectableSpan) : kFallbackInspectableSpan;
            if (offset >= 0 && offset < valid_span)
            {
                m_TypeTree.AssignByteOffset(typeItr, static_cast<uint32_t>(offset));
            }
        }
    }
    m_ActiveFather = typeItr;
}

void GenerateTypeTreeTransfer::EndTransfer()
{
    TypeTreeIterator current = m_ActiveFather;
    m_ActiveFather = m_ActiveFather.Father();
    if (!m_ActiveFather.IsNull())
    {
        m_ActiveFather.GetWritableNode(m_TypeTree)->m_ByteSize = current->m_ByteSize != -1 && m_ActiveFather->m_ByteSize != -1 ? m_ActiveFather->m_ByteSize + current->m_ByteSize : -1;

        if (current->m_MetaFlag & kAnyChildUsesAlignBytesFlag)
        {
            m_ActiveFather.GetWritableNode(m_TypeTree)->m_MetaFlag != kAnyChildUsesAlignBytesFlag;
        }
    }
}

void GenerateTypeTreeTransfer::BeginArrayTransfer(const char* name, const char* typeString, int32_t& size, TransferMetaFlags metaFlag)
{
    BeginTransfer(name, typeString, nullptr, metaFlag);
    m_ActiveFather.GetWritableNode(m_TypeTree)->AddTypeFlags(TypeTreeNode::TypeFlags::kFlagIsArray);

    Transfer(size, "size");
}

void GenerateTypeTreeTransfer::EndArrayTransfer()
{
    m_ActiveFather.GetWritableNode(m_TypeTree)->m_ByteSize = -1;
    EndTransfer();
}

void GenerateTypeTreeTransfer::Align()
{
    if (!m_ActiveFather.IsNull() && !m_ActiveFather.Children().IsNull())
    {
        m_ActiveFather.Children().Last().GetWritableNode(m_TypeTree)->m_MetaFlag |= kAlignBytesFlag;
        m_ActiveFather.GetWritableNode(m_TypeTree)->m_MetaFlag | kAnyChildUsesAlignBytesFlag;
    }
}