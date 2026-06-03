#include "TypeTree.h"

#include "Runtime/Function/CommonStringTable/CommonStringTable.h"

TypeTreeShareableData::TypeTreeShareableData()
    : m_Nodes(1), m_Levels(1), m_NextIndex(1)
{
    m_Nodes.back().Initialize(0);
    m_Levels.back() = 0;
    m_NextIndex.back() = -1;
}

TypeTreeShareableData::TypeTreeShareableData(const TypeTreeShareableData& src)
{
    m_Nodes = src.m_Nodes;
    m_Levels = src.m_Levels;
    m_NextIndex = src.m_NextIndex;
    m_StringBuffer = src.m_StringBuffer;
    m_ByteOffsets = src.m_ByteOffsets;
}

void TypeTreeShareableData::BlobWrite(std::vector<uint8_t>& cache) const
{
    size_t writeSize = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(TypeTreeNode) * m_Nodes.size() + m_StringBuffer.size();
    cache.resize(cache.size() + writeSize);
    uint8_t* cursor = cache.data() + cache.size() - writeSize;

    uint32_t* numberOfNodes = reinterpret_cast<uint32_t*>(cursor);
    cursor += sizeof(uint32_t);
    uint32_t* numberOfChars = reinterpret_cast<uint32_t*>(cursor);
    cursor += sizeof(uint32_t);
    TypeTreeNode* nodes = reinterpret_cast<TypeTreeNode*>(cursor);
    cursor += sizeof(TypeTreeNode) * m_Nodes.size();
    char* stringBuf = reinterpret_cast<char*>(cursor);

    *numberOfNodes = m_Nodes.size();
    *numberOfChars = m_StringBuffer.size();
    std::memcpy(nodes, m_Nodes.data(), sizeof(TypeTreeNode) * m_Nodes.size());
    std::memcpy(stringBuf, m_StringBuffer.data(), m_StringBuffer.size());
}

bool TypeTreeShareableData::BlobRead(const uint8_t*& iterator, const uint8_t* end)
{
    uint32_t numberOfNodes = LoadUnaligned<uint32_t>(iterator);
    iterator += sizeof(uint32_t);

    if (numberOfNodes == 0)
        return true;

    uint32_t numberOfChars = LoadUnaligned<uint32_t>(iterator);
    iterator += sizeof(uint32_t);

    if (iterator + numberOfNodes * sizeof(TypeTreeNode) + numberOfChars > end)
        return false;

    m_Nodes.resize(numberOfNodes);
    m_Levels.resize(numberOfNodes);
    m_NextIndex.resize(numberOfNodes);
    m_StringBuffer.resize(numberOfChars);

    std::memcpy(m_Nodes.data(), iterator, sizeof(TypeTreeNode) * numberOfNodes);
    for (size_t i = 0; i < numberOfNodes; i++)
    {
        m_Levels[i] = m_Nodes[i].m_Level;
        m_NextIndex[i] = -1;
    }
    iterator += sizeof(TypeTreeNode) * numberOfNodes;
    std::memcpy(m_StringBuffer.data(), iterator, numberOfChars);
    iterator += numberOfChars;

    return true;
}

TypeTree& TypeTree::operator=(const TypeTree& rhs)
{
    ReleaseSharedData();

    m_Data = rhs.m_Data;
    m_Data->Retain();

    return *this;
}

TypeTree::TypeTree()
{
    m_Data = MemoryManager::CreateObject<TypeTreeShareableData>();
}

TypeTree::TypeTree(TypeTreeShareableData* sharedType)
{
    sharedType->Retain();
    m_Data = sharedType;
}

TypeTree::~TypeTree()
{
    ReleaseSharedData();
}

void TypeTree::ReleaseSharedData()
{
    if (m_Data != nullptr)
    {
        m_Data->Release();
        m_Data = nullptr;
    }
}

const TypeTreeNode* TypeTreeIterator::GetNode() const
{
    return &m_TypeTreeData->Nodes()[m_NodeIndex];
}

void TypeTree::AssignTypeString(const TypeTreeIterator& i, const char* content)
{
    i.m_TypeTreeData->CreateString(i.GetWritableNode(*this)->m_TypeStrOffset, content);
}

void TypeTree::AssignNameString(const TypeTreeIterator& i, const char* content)
{
    i.m_TypeTreeData->CreateString(i.GetWritableNode(*this)->m_NameStrOffset, content);
}

void TypeTree::AssignByteOffset(TypeTreeIterator& i, uint32_t offset)
{
    i.m_TypeTreeData->SetByteOffset(i.m_NodeIndex, offset);
}

void TypeTree::BlobWrite(std::vector<uint8_t>& cache) const
{
    m_Data->BlobWrite(cache);
}

bool TypeTree::BlobRead(const uint8_t*& iterator, const uint8_t* end)
{
    return m_Data->BlobRead(iterator, end);
}

bool TypeTree::ReadTypeTree(TypeTree& t, const uint8_t*& iterator, const uint8_t* end)
{
    return t.BlobRead(iterator, end);
}

void TypeTree::WriteTypeTree(const TypeTree& t, std::vector<uint8_t>& cache)
{
    t.BlobWrite(cache);
}

static inline uint32_t MakeStringOffset(uint32_t offset, bool isCommonString)
{
    return offset | (isCommonString ? TypeTree::kCommonStringBit : 0);
}

void TypeTreeShareableData::CreateString(uint32_t& strOffset, const char* content)
{
    const char* str = GET_SYSTEM(CommonStringTable)->FindCommonString(content, strlen(content));
    if (str != nullptr)
    {
        strOffset = MakeStringOffset(str - CommonString::BufferBegin, true);
        return;
    }

    const char* begin = m_StringBuffer.data();
    const char* end = m_StringBuffer.data() + m_StringBuffer.size();
    for (const char* p = begin; p < end;)
    {
        size_t len = std::strlen(p);
        if (std::strcmp(p, content) == 0)
        {
            strOffset = MakeStringOffset(p - begin, false);
            return;
        }

        p = p + len + 1;
    }
    int len = std::strlen(content) + 1;
    m_StringBuffer.insert(m_StringBuffer.end(), content, content + len);
    strOffset = MakeStringOffset(m_StringBuffer.size() - len, false);
}

void TypeTreeShareableData::SetByteOffset(size_t nodeIndex, uint32_t offset)
{
    if (nodeIndex >= m_ByteOffsets.size())
        m_ByteOffsets.resize(nodeIndex + 1);
    m_ByteOffsets[nodeIndex] = offset;
}

TypeTreeIterator TypeTreeIterator::Father() const
{
    const TypeTreeNode* node = GetNode();
    const uint32_t fatherLevel = node->m_Level - 1;

    for (const TypeTreeNode* p = node - 1; p >= m_TypeTreeData->Nodes().data(); --p)
    {
        if (p->m_Level == fatherLevel)
            return TypeTreeIterator(*this, p - m_TypeTreeData->Nodes().data());
    }
    return TypeTreeIterator();
}

TypeTreeIterator TypeTreeIterator::Children() const
{
    const TypeTreeNode* node = GetNode();
    const TypeTreeNode* end = m_TypeTreeData->Nodes().data() + m_TypeTreeData->NodeCount();
    return node + 1 < end && (node + 1)->m_Level == node->m_Level + 1 ? TypeTreeIterator(*this, m_NodeIndex + 1) : TypeTreeIterator();
}

TypeTreeIterator TypeTreeIterator::Next() const
{
    const TypeTreeNode* node = GetNode();
    const uint32_t siblingLevel = node->m_Level;
    const TypeTreeNode* end = m_TypeTreeData->Nodes().data() + m_TypeTreeData->Nodes().size();

    size_t nodeIndex = node - m_TypeTreeData->Nodes().data();
    const int32_t* nextIndexConst = &m_TypeTreeData->NextIndex().data()[nodeIndex];
    int32_t* nextIndex = const_cast<int32_t*>(nextIndexConst);

    if (*nextIndex >= 0)
    {
        return TypeTreeIterator(*this, *nextIndex);
    }
    if (*nextIndex == -2)
    {
        return TypeTreeIterator();
    }
    const uint8_t* level = &m_TypeTreeData->Levels().data()[nodeIndex + 1];
    for (const TypeTreeNode* p = node + 1; p < end; ++p, ++level)
    {
        if (*level > siblingLevel)
            continue;

        if (*level == siblingLevel)
        {
            *nextIndex = p - m_TypeTreeData->Nodes().data();
            return TypeTreeIterator(*this, *nextIndex);
        }
        *nextIndex = -2;
        return TypeTreeIterator();
    }

    return TypeTreeIterator();
}

TypeTreeIterator TypeTreeIterator::Last() const
{
    TypeTreeIterator i = *this;
    TypeTreeIterator next;
    while (true)
    {
        next = i.Next();
        if (next.IsNull())
            break;
        i = next;
    }
    return i;
}

static const char* CalculateString(uint32_t offset, const char* stringBuffer)
{
    return ((offset & TypeTree::kCommonStringBit) ? CommonString::BufferBegin : stringBuffer) + (offset & TypeTree::kStringOffsetMask);
}

TypeTreeString TypeTreeIterator::Type() const
{
    TypeTreeString str;
    str.m_Buffer = CalculateString(GetNode()->m_TypeStrOffset, m_TypeTreeData->StringsBuffer().data());
    return str;
}

TypeTreeString TypeTreeIterator::Name() const
{
    TypeTreeString str;
    str.m_Buffer = CalculateString(GetNode()->m_NameStrOffset, m_TypeTreeData->StringsBuffer().data());
    return str;
}