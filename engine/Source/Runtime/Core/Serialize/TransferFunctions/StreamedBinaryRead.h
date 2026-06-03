#pragma once
#include "Runtime/Core/Serialize/SerializationCaching/CachedReader.h"
#include "Runtime/Core/Serialize/TransferFunctions/TransferBase.h"
class StreamedBinaryRead : public TransferBase
{
public:
    static constexpr bool IsReading() noexcept { return true; }
    static constexpr bool IsWriting() noexcept { return false; }

    CachedReader& Init(TransferInstructionFlags flags) { return m_Cache; }
    template<typename T>
    void TransferBase(T& data, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags);

    template<typename T>
    void Transfer(T& data, const char* name, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags);

    void ReadDirect(void* data, int byteSize);

    void Align();

    template<typename T>
    void TransferBasicData(T& data);

    template<typename T>
    void TransferArray(T& data, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags);

    template<typename T>
    void TransferMap(T& data, TransferMetaFlags metaFlag = kNoTransferFlags);

    template<typename T>
    void TransferStringData(T& data);

private:
    CachedReader m_Cache;
};

template<typename T>
void StreamedBinaryRead::TransferBase(T& data, TransferMetaFlags metaFlag)
{
    Transfer(data, kTransferNameIdentifierBase, metaFlag);
}

template<typename T>
void StreamedBinaryRead::Transfer(T& data, const char* name, TransferMetaFlags metaFlag)
{
    SerializeTraits<T>::Transfer(data, *this);
}

template<typename T>
inline void StreamedBinaryRead::TransferBasicData(T& data)
{
    m_Cache.Read(data);
}

template<typename T>
void StreamedBinaryRead::TransferArray(T& data, TransferMetaFlags)
{
    int32_t size;
    Transfer(size, "size");
    SerializeTraits<T>::ResizeArray(data, size);

    if (SerializeTraits<typename T::value_type>::AllowTransferOptimization() && SerializeTraits<T>::IsContinousMemoryArray())
    {
        if (size != 0)
        {
            ReadDirect(&*data.data(), size * sizeof(typename T::value_type));
        }
    }
    else
    {
        typename T::iterator i;
        typename T::iterator end = data.end();
        for (i = data.begin(); i != end; ++i)
            Transfer(*i, "data");
    }
}

template<typename T>
void StreamedBinaryRead::TransferMap(T& data, TransferMetaFlags metaFlag)
{
    int32_t size;
    Transfer(size, "size");

    typename NonConstContainerValueType<T>::value_type p;

    ContainerClear(data);

    for (int i = 0; i < size; i++)
    {
        Transfer(p, "data");
        data.insert(std::move(p));
    }
}

template<typename T>
void StreamedBinaryRead::TransferStringData(T& data)
{
    TransferArray(data);
}