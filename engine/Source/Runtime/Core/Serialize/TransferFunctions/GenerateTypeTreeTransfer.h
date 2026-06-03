#pragma once
#include "Runtime/Core/Serialize/SerializationMetaFlags.h"
#include "Runtime/Core/Serialize/SerializeTraitsBase.h"
#include "Runtime/Core/Serialize/TypeTree.h"
#include "TransferBase.h"

class GenerateTypeTreeTransfer : public TransferBase
{
public:
    // Schema-only transfer: we are neither reading nor writing object
    // data. PPtr<T>::Transfer still descends into m_FileID/m_PathID
    // here so the TypeTree contains both child nodes, but skips any
    // resolver round-trip.
    static constexpr bool IsReading() noexcept { return false; }
    static constexpr bool IsWriting() noexcept { return false; }

    GenerateTypeTreeTransfer(TypeTree& t, TransferInstructionFlags flags, void* objectPtr, int objectSize);

    template<typename T>
    void TransferBase(T& data, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags);
    template<typename T>
    void Transfer(T& data, const char* name, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags);
    template<typename T>
    void TransferBasicData(T& data);
    template<typename T>
    void TransferStringData(T& data);
    template<typename T>
    void TransferArray(T& data, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags);

    template<typename T>
    void TransferMap(T& data, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags);

    template<typename T>
    void TransferArrayWithElement(T& elementType, TransferMetaFlags metaFlag);

    void Align();

    void BeginTransfer(const char* name, const char* typeString, char* data, TransferMetaFlags metaFlag);
    void EndTransfer();

private:
    void BeginArrayTransfer(const char* name, const char* typeString, int32_t& size, TransferMetaFlags metaFlag);
    void EndArrayTransfer();
    TypeTree& m_TypeTree;
    TypeTreeIterator m_ActiveFather;
    char* m_ObjectPtr;
    int m_ObjectSize;
    int m_Index;
};

template<typename T>
void GenerateTypeTreeTransfer::TransferBase(T& data, TransferMetaFlags metaFlag)
{
    Transfer(data, kTransferNameIdentifierBase, metaFlag);
}

template<typename T>
inline void GenerateTypeTreeTransfer::Transfer(T& data, const char* name, TransferMetaFlags metaFlag /* = TransferMetaFlag::None */)
{
    BeginTransfer(name, SerializeTraits<T>::GetTypeString(&data), (char*)&data, metaFlag);
    SerializeTraits<T>::Transfer(data, *this);
    EndTransfer();
}

template<typename T>
inline void GenerateTypeTreeTransfer::TransferStringData(T& data)
{
    TransferArray(data, kNoTransferFlags);
}

template<typename T>
inline void GenerateTypeTreeTransfer::TransferArray(T& data, TransferMetaFlags metaFlag)
{
    typedef typename T::value_type ElementType;

    ElementType* element = MemoryManager::CreateObject<ElementType>();
    TransferArrayWithElement(*element, metaFlag);
    MemoryManager::DestroyObject(element);
}

template<typename T>
inline void GenerateTypeTreeTransfer::TransferArrayWithElement(T& elementType, TransferMetaFlags metaFlag)
{
    int32_t size;
    BeginArrayTransfer("Array", "Array", size, metaFlag);

    Transfer(elementType, "data");

    EndArrayTransfer();
}

template<typename T>
void GenerateTypeTreeTransfer::TransferMap(T& data, TransferMetaFlags metaFlag)
{
    int32_t size;
    BeginArrayTransfer("Array", "Array", size, metaFlag);

    typename NonConstContainerValueType<T>::value_type p;
    Transfer(p, "data");

    EndArrayTransfer();
}

template<typename T>
inline void GenerateTypeTreeTransfer::TransferBasicData(T&)
{
    auto&& node = m_ActiveFather.GetWritableNode(m_TypeTree);
    node->m_ByteSize = SerializeTraits<T>::GetByteSize();
}