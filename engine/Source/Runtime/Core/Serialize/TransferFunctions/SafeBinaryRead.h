#pragma once

#include "Runtime/Core/Serialize/SerializationCaching/CachedReader.h"
#include "Runtime/Core/Serialize/TypeTree.h"
#include "Runtime/Utility/ContainerUtility.h"
#include "Runtime/VirtualFileSystem/VirtualFileSystemTypes.h"
#include "TransferBase.h"

class SafeBinaryRead;

using ConversionFunction = bool(void* inData, SafeBinaryRead& transfer);

class SafeBinaryRead : public TransferBase
{
public:
    static constexpr bool IsReading() noexcept { return true; }
    static constexpr bool IsWriting() noexcept { return false; }

    CachedReader& Init(const TypeTreeIterator& oldBase, FileSize bytePosition, int64_t byteSize, TransferInstructionFlags flags);

    template<typename T>
    void TransferBase(T& data, TransferMetaFlags metaFlag = kNoTransferFlags);
    template<typename T>
    void Transfer(T& data, const char* name, TransferMetaFlags metaFlag = kNoTransferFlags);

    template<typename T>
    void TransferWithTypeString(T& data, const char* name, const char* typeName, TransferMetaFlags metaFlag = kNoTransferFlags);

    template<typename T>
    void TransferBasicData(T& data);

    template<typename T>
    void TransferStringData(T& data);

    template<typename T>
    void TransferArray(T& data, TransferMetaFlags metaFlag = kNoTransferFlags);

    template<typename T>
    void TransferMap(T& data, TransferMetaFlags metaFlag = kNoTransferFlags);

    int BeginTransfer(const char* name, const char* typeString, ConversionFunction** converter, bool allowTypeConversion);
    void EndTransfer();

private:
    bool BeginArrayTransfer(const char* name, const char* typeString, int32_t& size);

    void EndArrayTransfer();

    void Walk(const TypeTreeIterator& type, FileSize* bytePosition);

    struct ArrayPositionInfo
    {
        int32_t arrayPosition;
        FileSize cachedBytePosition;
        int32_t cachedArrayPosition;
    };

    std::vector<ArrayPositionInfo> m_PositionInArray;

    CachedReader m_Cache;
    FileSize m_BaseBytePosition;
    int64_t m_BaseByteSize;

    TypeTreeIterator m_OldBaseType;
    enum
    {
        kNotFound = 0,
        kMatchesType = 1,
        kFastPathKnownByteSizeArrayType = 2,
        kNeedConversion = -1
    };
    struct StackedInfo
    {
        TypeTreeIterator type;
        const char* currentTypeName;
        FileSize bytePosition;
        int version;

        FileSize cachedBytePosition;
        TypeTreeIterator cachedIterator;
    };
    StackedInfo* m_CurrentStackInfo;
    int32_t* m_CurrentPositionInArray;
    std::vector<StackedInfo> m_StackInfo;
    bool m_DidReadLastProperty;
};

template<typename T>
void SafeBinaryRead::TransferBase(T& data, TransferMetaFlags metaFlags)
{
    Transfer(data, kTransferNameIdentifierBase, metaFlags);
}

template<typename T>
inline void SafeBinaryRead::Transfer(T& data, const char* name, TransferMetaFlags metaFlag)
{
    TransferWithTypeString(data, name, SerializeTraits<T>::GetTypeString(&data), kNoTransferFlags);
}

template<typename T>
inline void SafeBinaryRead::TransferWithTypeString(T& data, const char* name, const char* typeName, TransferMetaFlags)
{
    ConversionFunction* converter;
    int conversion = BeginTransfer(name, typeName, &converter, SerializeTraits<T>::AllowTypeConversion());
    if (conversion == kNotFound)
        return;

    if (conversion >= kMatchesType)
        SerializeTraits<T>::Transfer(data, *this);
    EndTransfer();
}

template<typename T>
void SafeBinaryRead::TransferBasicData(T& data)
{
    m_Cache.Read(data, m_CurrentStackInfo->bytePosition.Cast<size_t>());
}

template<typename T>
void SafeBinaryRead::TransferStringData(T& data)
{
    TransferArray(data);
}

template<typename T>
void SafeBinaryRead::TransferArray(T& data, TransferMetaFlags flag)
{
    int32_t size = data.size();
    if (!BeginArrayTransfer("Array", "Array", size))
        return;

    SerializeTraits<T>::ResizeArray(data, size);

    typename T::iterator i;
    typename T::iterator end = data.end();
    if (size != 0)
    {
        int conversion = BeginTransfer("data", SerializeTraits<typename T::value_type>::GetTypeString(&*data.begin()), nullptr, SerializeTraits<typename T::value_type>::AllowTypeConversion());

        size_t elementSize = m_CurrentStackInfo->type->m_ByteSize;
        *m_CurrentPositionInArray = 0;

        if (conversion == kFastPathKnownByteSizeArrayType)
        {
            FileSize basePosition = m_CurrentStackInfo->bytePosition;
            for (i = data.begin(); i != end; ++i)
            {
                FileSize currentBytePosition = basePosition + (*m_CurrentPositionInArray) * elementSize;
                m_CurrentStackInfo->cachedBytePosition = currentBytePosition;
                m_CurrentStackInfo->bytePosition = currentBytePosition;
                m_CurrentStackInfo->cachedIterator = m_CurrentStackInfo->type.Children();
                (*m_CurrentPositionInArray)++;
                SerializeTraits<typename T::value_type>::Transfer(*i, *this);
            }
            EndTransfer();
        }
        else
        {
            EndTransfer();
            for (i = data.begin(); i != end; ++i)
            {
                Transfer(*i, "data");
            }
        }
    }

    EndArrayTransfer();
}

template<typename T>
void SafeBinaryRead::TransferMap(T& data, TransferMetaFlags metaFlag)
{
    int32_t size = data.size();
    if (!BeginArrayTransfer("Array", "Array", size))
        return;

    typename NonConstContainerValueType<T>::value_type p;

    ContainerClear(data);
    for (int i = 0; i < size; i++)
    {
        Transfer(p, "data");
        data.insert(p);
    }
    EndArrayTransfer();
}