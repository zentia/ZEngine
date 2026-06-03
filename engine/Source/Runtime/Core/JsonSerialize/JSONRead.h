#pragma once
#include "JSONAllocator.h"
#include "JSONSerializeTraits.h"
#include "Runtime/Core/Serialize/SerializationMetaFlags.h"
#include "Runtime/Core/Serialize/TransferFunctions/TextTransferReadBase.h"
#include "rapidjson/document.h"

class JSONRead;
template<>
struct TextTransferTraits<JSONRead>
{
    using InternalNode = rapidjson::GenericValue<rapidjson::UTF8<>, JSONAllocator>;
    using InternalNodeRef = const InternalNode*;
};
class JSONRead : public TextTransferReadBase<JSONRead>
{
public:
    using Document = rapidjson::GenericDocument<rapidjson::UTF8<>, JSONAllocator, JSONAllocator>;
    using Value = TextTransferTraits<JSONRead>::InternalNode;

    JSONRead(const char* strBuffer, TransferInstructionFlags flags);

    // Static dispatch hooks consumed by templates such as PPtr<T>::Transfer
    // via `if constexpr`. JSONRead is a read transfer.
    static constexpr bool IsReading() noexcept { return true; }
    static constexpr bool IsWriting() noexcept { return false; }

    template<typename T>
    void
    Transfer(T& data, const char* name, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags, bool isRootBase = false);

    template<typename T>
    void TransferBasicData(T& data);

    template<typename T>
    void TransferStringData(T& data);

    template<typename T>
    void TransferArray(T& data, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags);

    template<typename T>
    void TransferMap(T& data, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags);

    template<typename T>
    void TransferSet(T& data, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags);

    template<typename T>
    void TransferMapAsObject(T& data, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags);

    template<typename T>
    void
    TransferPair(T& data, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags, const rapidjson::Value* pair = nullptr);

    void TransferTypelessData(uint32_t szie, void* data, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags);

    void ResetCurrentNodeToOriginalState();

private:
    const Value* GetValueForKey(const Value* parentNode, const char* keystr);
    const Value* GetValueForKeyWithNameConversion(const char* type, const Value* parentNode, const char* name);
    void Init(TransferInstructionFlags flags);
    Document m_Document;
    JSONAllocator m_Allocator;
};

template<>
inline void JSONRead::TransferBasicData<bool>(bool& data)
{
    if (m_CurrentNode->IsBool())
        data = m_CurrentNode->GetBool();
    else if (m_CurrentNode->IsString())
        data = m_CurrentNode->GetString() == "true";
    else if (m_CurrentNode->IsNumber())
        data = m_CurrentNode->GetDouble() != 0;
    else
        data = false;
}

template<>
inline void JSONRead::TransferBasicData<int32_t>(int32_t& data)
{
    if (m_CurrentNode->IsInt())
        data = m_CurrentNode->GetInt();
    else if (m_CurrentNode->IsNumber())
        data = (uint32_t)m_CurrentNode->GetDouble();
    else if (m_CurrentNode->IsString())
        data = std::stoi(m_CurrentNode->GetString());
    else
        data = 0;
}

template<>
inline void JSONRead::TransferBasicData<int16_t>(int16_t& data)
{
    int32_t data32;
    TransferBasicData<int32_t>(data32);
    data = (int16_t)data32;
}

template<>
inline void JSONRead::TransferBasicData<int8_t>(int8_t& data)
{
    int32_t data32;
    TransferBasicData<int32_t>(data32);
    data = (int8_t)data32;
}

template<>
inline void JSONRead::TransferBasicData<char>(char& data)
{
    if (m_CurrentNode->IsString() && m_CurrentNode->GetStringLength() == 1)
        data = m_CurrentNode->GetString()[0];
    else
        TransferBasicData<int8_t>(reinterpret_cast<int8_t&>(data));
}

template<>
inline void JSONRead::TransferBasicData<uint32_t>(uint32_t& data)
{
    if (m_CurrentNode->IsUint())
        data = m_CurrentNode->GetUint();
    else if (m_CurrentNode->IsNumber())
        data = (uint32_t)m_CurrentNode->GetDouble();
    else
        data = 0;
}

template<>
inline void JSONRead::TransferBasicData<uint16_t>(uint16_t& data)
{
    uint32_t data32;
    TransferBasicData<uint32_t>(data32);
    data = (uint16_t)data32;
}

template<>
inline void JSONRead::TransferBasicData<uint8_t>(uint8_t& data)
{
    uint32_t data32;
    TransferBasicData<uint32_t>(data32);
    data = (uint8_t)data32;
}

template<>
inline void JSONRead::TransferBasicData<int64_t>(int64_t& data)
{
    if (m_CurrentNode->IsInt64())
        data = m_CurrentNode->GetInt64();
    else if (m_CurrentNode->IsNumber())
        data = m_CurrentNode->GetDouble();
    else if (m_CurrentNode->IsString())
        data = std::stoll(m_CurrentNode->GetString());
}

template<>
inline void JSONRead::TransferBasicData<uint64_t>(uint64_t& data)
{
    if (m_CurrentNode->IsUint64())
        data = m_CurrentNode->GetUint64();
}

template<>
inline void JSONRead::TransferBasicData<double>(double& data)
{
    if (m_CurrentNode->IsNumber())
        data = m_CurrentNode->GetDouble();
    else if (m_CurrentNode->IsString())
        data = std::stod(m_CurrentNode->GetString());
}

template<>
inline void JSONRead::TransferBasicData<float>(float& data)
{
    if (m_CurrentNode->IsNumber())
        data = m_CurrentNode->GetDouble();
    else if (m_CurrentNode->IsString())
        data = std::stof(m_CurrentNode->GetString());
}

template<class T>
inline void JSONRead::TransferStringData(T& data)
{
    if (m_CurrentNode->IsString())
        data = m_CurrentNode->GetString();
    else if (m_CurrentNode->IsDouble())
        data = eastl::to_string(m_CurrentNode->GetDouble());
    else
        data = "";
}

template<typename T>
void JSONRead::Transfer(T& data, const char* name, TransferMetaFlags metaFlag, bool isRootBase)
{
    const Value* parentNode = m_CurrentNode;
    if (name != nullptr && (parentNode == nullptr || !parentNode->IsObject()))
        return;

    const char* typeForNameConversion = isRootBase ? SerializeTraits<T>::GetTypeString(&data) : m_CurrentType;

    m_CurrentNode = GetValueForKeyWithNameConversion(typeForNameConversion, parentNode, name);
    if (m_CurrentNode == nullptr)
    {
        if (isRootBase)
        {
            LOG_FATAL(ZSerializer, "Not implemented yet");
        }
    }

    const char* parentType = m_CurrentType;
    m_CurrentType = SerializeTraits<T>::GetTypeString(&data);

    if (m_CurrentNode != nullptr)
    {
        JSONSerializeTraits<T>::Transfer(data, *this);
    }

    m_CurrentNode = parentNode;
    m_CurrentType = parentType;
}

template<typename T>
void JSONRead::TransferArray(T& data, TransferMetaFlags metaFlag)
{
    const Value* parentNode = m_CurrentNode;
    using non_const_value_type = typename NonConstContainerValueType<T>::value_type;

    if (m_CurrentNode->IsNull())
    {
        SerializeTraits<T>::ResizeArray(data, 0);
        return;
    }

    if (m_CurrentNode->GetType() != rapidjson::kArrayType)
        return;

    SerializeTraits<T>::ResizeArray(data, m_CurrentNode->Size());

    const Value* start = m_CurrentNode->Begin();
    const Value* top = m_CurrentNode->End();

    typename T::iterator dataIterator = data.begin();

    for (const Value* i = start; i != top; i++)
    {
        m_CurrentNode = i;
        non_const_value_type& itemData = *dataIterator;
        JSONSerializeTraits<non_const_value_type>::Transfer(itemData, *this);
        ++dataIterator;
    }
    m_CurrentNode = parentNode;
}

template<typename T>
void JSONRead::TransferMap(T& data, TransferMetaFlags metaFlag)
{
}

template<typename T>
void JSONRead::TransferMapAsObject(T& data, TransferMetaFlags metaFlag)
{
}

template<typename T>
void JSONRead::TransferPair(T& data, TransferMetaFlags metaFlag, const rapidjson::Value* pair)
{
}