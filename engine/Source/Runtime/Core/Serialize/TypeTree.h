#pragma once
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Function/CommonStringTable/CommonString.h"
#include "SerializationMetaFlags.h"

class TypeTree;
class TypeTreeNode;
class TypeTreeIterator;

class TypeTreeString
{
public:
    inline bool operator==(const char* str) const
    {
        if (str == m_Buffer)
            return true;

        if (!str || !m_Buffer)
            return false;

        if (IsCommonString(m_Buffer) && IsCommonString(str))
            return false;

        return std::strcmp(str, m_Buffer) == 0;
    }

    const char* c_str() const { return m_Buffer != nullptr ? m_Buffer : ""; }

private:
    friend class TypeTreeIterator;
    const char* m_Buffer;
};

class TypeTreeNode
{
public:
    enum TypeFlags
    {
        kFlagNone = 0,
        kFlagIsArray = 1 << 0,
        IsArrayOfRefs = 1 << 1,
    };
    void Initialize(uint8_t inLevel, uint64_t inRefTypeHash = 0)
    {
        m_Level = inLevel;
        m_NameStrOffset = 0;
        m_TypeStrOffset = 0;
        m_Index = -1;
        m_TypeFlags = 0;
        m_Version = 1;
        m_MetaFlag = TransferMetaFlags::kNoTransferFlags;
        m_ByteSize = -1;
        refTypeHash = inRefTypeHash;
    }
    void inline AddTypeFlags(TypeFlags flags) { m_TypeFlags |= (uint8_t)flags; }
    bool inline IsArray() const { return m_TypeFlags & kFlagIsArray; }
    int16_t m_Version;
    uint8_t m_Level;
    uint8_t m_TypeFlags;
    uint32_t m_TypeStrOffset;
    uint32_t m_NameStrOffset;
    int32_t m_ByteSize;
    int32_t m_Index;
    TransferMetaFlags m_MetaFlag;

    uint64_t refTypeHash;
};

class TypeTreeShareableData
{
public:
    TypeTreeShareableData();
    TypeTreeShareableData(const TypeTreeShareableData& src);

    TypeTreeNode* GetWritable(size_t nodeIndex) const { return const_cast<TypeTreeNode*>(&m_Nodes[nodeIndex]); }

    const std::vector<TypeTreeNode>& Nodes() const { return m_Nodes; }
    size_t NodeCount() const { return m_Nodes.size(); }

    const std::vector<uint8_t>& Levels() const { return m_Levels; }

    const std::vector<int32_t>& NextIndex() const { return m_NextIndex; }

    bool HasByteOffset(size_t nodeIndex) const { return nodeIndex < m_ByteOffsets.size(); }
    uint32_t GetByteOffset(size_t nodeIndex) const { return HasByteOffset(nodeIndex) ? m_ByteOffsets[nodeIndex] : 0; }

    size_t AddChildNode(size_t fatherIndex)
    {
        TypeTreeNode& newNode = m_Nodes.emplace_back();
        newNode.Initialize(m_Nodes[fatherIndex].m_Level + 1);
        m_Levels.emplace_back(newNode.m_Level);
        m_NextIndex.emplace_back(-1);
        return m_Nodes.size() - 1;
    }

    void CreateString(uint32_t& strOffset, const char* content);
    void SetByteOffset(size_t nodeIndex, uint32_t offset);

    const std::vector<char>& StringsBuffer() const { return m_StringBuffer; }

    void BlobWrite(std::vector<uint8_t>& cache) const;
    bool BlobRead(const uint8_t*& iterator, const uint8_t* end);

    void Retain() { m_RefCount++; }
    void Release()
    {
        m_RefCount--;
        if (m_RefCount.load() == 0)
        {
            MemoryManager::DestroyObject(this);
        }
    }

private:
    std::vector<TypeTreeNode> m_Nodes;
    std::vector<uint8_t> m_Levels;
    std::vector<int32_t> m_NextIndex;
    std::vector<char> m_StringBuffer;

    std::vector<uint32_t> m_ByteOffsets;

    std::atomic<int> m_RefCount {1};
};

class TypeTreeIterator
{
public:
    TypeTreeIterator()
        : m_LinkedTypeTree(nullptr), m_TypeTreeData(nullptr), m_NodeIndex(0) {}
    TypeTreeIterator(const TypeTree* typeTree, TypeTreeShareableData* data, size_t nodeIndex)
        : m_LinkedTypeTree(typeTree), m_TypeTreeData(data), m_NodeIndex(nodeIndex)
    {
    }
    TypeTreeIterator(const TypeTreeIterator& reference, size_t nodeIndex)
        : m_LinkedTypeTree(reference.m_LinkedTypeTree), m_TypeTreeData(reference.m_TypeTreeData), m_NodeIndex(nodeIndex)
    {
    }
    inline bool IsNull() const { return m_TypeTreeData == nullptr; }
    const TypeTreeNode* operator->() const { return GetNode(); }

    TypeTreeIterator Father() const;
    TypeTreeIterator Children() const;
    TypeTreeIterator Next() const;
    TypeTreeIterator Last() const;

    TypeTreeString Type() const;
    TypeTreeString Name() const;

    inline int32_t ByteSize() const { return GetNode()->m_ByteSize; }
    inline TransferMetaFlags MetaFlags() const { return (TransferMetaFlags)(GetNode()->m_MetaFlag); }
    inline bool HasByteOffset() const { return m_TypeTreeData != nullptr && m_TypeTreeData->HasByteOffset(m_NodeIndex); }
    inline uint32_t ByteOffset() const { return m_TypeTreeData != nullptr ? m_TypeTreeData->GetByteOffset(m_NodeIndex) : 0; }

    inline bool operator==(const TypeTreeIterator& rhs) const { return m_LinkedTypeTree == rhs.m_LinkedTypeTree && m_TypeTreeData == rhs.m_TypeTreeData && m_NodeIndex == rhs.m_NodeIndex; }
    inline bool operator!=(const TypeTreeIterator& rhs) const { return !(*this == rhs); }

    TypeTreeIterator AddChildNode(TypeTree& mutableType) const
    {
        TypeTreeShareableData& stt = *const_cast<TypeTreeShareableData*>(m_TypeTreeData);
        size_t index = stt.AddChildNode(m_NodeIndex);
        return TypeTreeIterator(*this, index);
    }
    TypeTreeNode* GetWritableNode(TypeTree& mutableType) const { return m_TypeTreeData->GetWritable(m_NodeIndex); }

private:
    friend class TypeTree;

    const TypeTreeNode* GetNode() const;

    const TypeTree* m_LinkedTypeTree;
    TypeTreeShareableData* m_TypeTreeData;
    size_t m_NodeIndex;
};

class TypeTree
{
public:
    enum
    {
        kCommonStringBit = 0x80000000U,
        kStringOffsetMask = ~kCommonStringBit
    };
    TypeTree();
    explicit TypeTree(TypeTreeShareableData* sharedType);
    ~TypeTree();

    TypeTree& operator=(const TypeTree& rhs);

    TypeTreeIterator Root() const { return TypeTreeIterator(this, m_Data, 0); }

    void AssignTypeString(const TypeTreeIterator& i, const char* content);
    void AssignNameString(const TypeTreeIterator& i, const char* content);
    void AssignByteOffset(TypeTreeIterator& i, uint32_t offset);

    void BlobWrite(std::vector<uint8_t>& cache) const;
    bool BlobRead(const uint8_t*& iterator, const uint8_t* end);

    static bool ReadTypeTree(TypeTree& t, const uint8_t*& iterator, const uint8_t* end);
    static void WriteTypeTree(const TypeTree& t, std::vector<uint8_t>& cache);

private:
    void ReleaseSharedData();
    TypeTreeShareableData* m_Data;
};