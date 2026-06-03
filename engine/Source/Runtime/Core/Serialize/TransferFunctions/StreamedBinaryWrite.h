#pragma once

#include "Runtime/Core/Serialize/SerializationCaching/CachedWriter.h"
#include "Runtime/Core/Serialize/TransferFunctions/TransferBase.h"

class StreamedBinaryWrite : public TransferBase
{
public:
    // PPtr<T>::Transfer dispatches on these. Static constexpr lets the
    // call site fold to a single branch via `if constexpr`. Mirrors
    // Unity's per-transfer-class IsWriting()/IsReading() contract --
    // ZEngine's TransferBase deliberately doesn't expose them as
    // virtuals because every other call site can resolve them at
    // compile time too.
    static constexpr bool IsReading() noexcept { return false; }
    static constexpr bool IsWriting() noexcept { return true; }

    CachedWriter& Init();
    CachedWriter& Init(const CachedWriter& cachedWriter);

    template<typename T>
    void TransferBase(T& data, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags);

    template<typename T>
    void Transfer(T& data, const char* name, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags);

    template<typename T>
    void TransferBasicData(T& data);

    template<typename T>
    void TransferStringData(T& data, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags);

    template<typename T>
    void TransferArray(T& data, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags);

    template<typename T>
    void TransferMap(T& data, TransferMetaFlags metaFlag = kNoTransferFlags);

    void Align();

private:
    CachedWriter m_Cache;
};

template<typename T>
void StreamedBinaryWrite::TransferBase(T& data, TransferMetaFlags metaFlag)
{
    Transfer(data, "Base", metaFlag);
}

template<typename T>
void StreamedBinaryWrite::Transfer(T& data, const char* name, TransferMetaFlags metaFlag)
{
    SerializeTraits<T>::Transfer(data, *this);
}

template<typename T>
void StreamedBinaryWrite::TransferStringData(T& data, TransferMetaFlags metaFlag)
{
    TransferArray(data, metaFlag);
}

template<typename T>
void StreamedBinaryWrite::TransferBasicData(T& data)
{
    m_Cache.Write(data);
}

template<typename T>
void StreamedBinaryWrite::TransferArray(T& data, TransferMetaFlags metaFlag)
{
    const T& cdata = (const T&)data;

    int32_t size = (int32_t)cdata.size();
    Transfer(size, "size");

    using non_const_value_type = typename T::value_type;

    if (size && SerializeTraits<T>::IsContinousMemoryArray() && SerializeTraits<typename T::value_type>::AllowTransferOptimization())
    {
        m_Cache.Write(&(*cdata.begin()), size * sizeof(typename T::value_type));
    }
    else
    {
        typename T::const_iterator end = cdata.end();
        for (typename T::const_iterator i = cdata.begin(); i != end; ++i)
        {
            non_const_value_type& p = (non_const_value_type&)(*i);
            Transfer(p, "data");
        }
    }
}

template<typename T>
void StreamedBinaryWrite::TransferMap(T& data, TransferMetaFlags metaFlag)
{
    int32_t size = data.size();
    Transfer(size, "size");

    using value_type = typename NonConstContainerValueType<T>::value_type;

    for (const auto& pair : data)
    {
        value_type& p = (value_type&)(pair);
        Transfer(p, "data");
    }
}