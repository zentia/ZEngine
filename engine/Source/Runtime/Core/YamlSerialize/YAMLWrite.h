#pragma once

#include "Runtime/Core/JsonSerialize/JSONAllocator.h"
#include "Runtime/Core/Serialize/TransferFunctions/TextTransferWriteBase.h"
#include "Runtime/Core/YamlSerialize/YAMLSerializeTraits.h"
#include "Runtime/Utility/Word.h"

#include "rapidjson/document.h"

// Block-YAML write transfer. Structurally identical to JSONWrite -- it builds the
// exact same rapidjson DOM through the shared TextTransferWriteBase machinery; the
// ONLY difference is OutputToString, which encodes the DOM as YAML rather than
// JSON (see YamlText.cpp). Keeping the two backends node-for-node identical means
// anything that serializes to JSON serializes to YAML with no per-type work.
class YAMLWrite;

template<>
struct TextTransferTraits<YAMLWrite>
{
    typedef rapidjson::GenericValue<rapidjson::UTF8<>, JSONAllocator> InternalNode;
    typedef InternalNode MetaParentNodeType;
    typedef InternalNode* InternalNodeRef;
    static const InternalNodeRef kInvalidNode;
};

class YAMLWrite : public TextTransferWriteBase<YAMLWrite>
{
public:
    typedef rapidjson::GenericDocument<rapidjson::UTF8<>, JSONAllocator, JSONAllocator> Document;
    typedef TextTransferTraits<YAMLWrite>::InternalNode Value;

private:
    JSONAllocator m_Allocator;
    Document m_Document;

    void TransferStringToCurrentNode(const char* str);
    void AppendToNode(Value* parentNode, const char* keyStr, Value& valueNode);

    friend class TextTransferWriteBase<YAMLWrite>;

public:
    YAMLWrite(TransferInstructionFlags flags);

    static constexpr bool IsReading() noexcept { return false; }
    static constexpr bool IsWriting() noexcept { return true; }

    template<typename T>
    void Transfer(T& data, const char* name, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags);

    template<typename T>
    void TransferBasicData(T& data);

    template<typename T>
    void TransferArray(T& data, TransferMetaFlags metaFlag = kNoTransferFlags);

    template<typename T>
    void TransferMap(T& data, TransferMetaFlags metaFlag = kNoTransferFlags);

    // prettyPrint is accepted for API symmetry with JSONWrite; block YAML is always
    // "pretty" so the flag has no effect.
    void OutputToString(eastl::string& str, bool prettyPrint = false);

    // Direct access to the underlying rapidjson DOM root. Used by
    // YamlObjectGraph to splice a single object's serialized fields into a
    // larger multi-object document (deep-copied across allocators) instead of
    // round-tripping through a YAML string per object.
    Document& GetDocument() { return m_Document; }
    const Document& GetDocument() const { return m_Document; }

    template<typename T>
    void TransferPair(T& data, TransferMetaFlags metaFlag = kNoTransferFlags);
};

template<>
inline void YAMLWrite::TransferBasicData<bool>(bool& data)
{
    m_CurrentNode->SetBool(data);
}

template<>
inline void YAMLWrite::TransferBasicData<float>(float& data)
{
    m_CurrentNode->SetDouble(data);
}

template<>
inline void YAMLWrite::TransferBasicData<char>(char& data)
{
    m_CurrentNode->SetUint(data);
}

template<>
inline void YAMLWrite::TransferBasicData<int8_t>(int8_t& data)
{
    if (!m_MetaFlags.empty() && (m_MetaFlags.back() & kTreatIntegerValueAsBoolean))
    {
        m_CurrentNode->SetBool(data != 0);
    }
    else
    {
        m_CurrentNode->SetUint(data);
    }
}

template<>
inline void YAMLWrite::TransferBasicData<int32_t>(int32_t& data)
{
    m_CurrentNode->SetUint(data);
}

template<>
inline void YAMLWrite::TransferBasicData<uint32_t>(uint32_t& data)
{
    m_CurrentNode->SetUint(data);
}

template<>
inline void YAMLWrite::TransferBasicData<uint16_t>(uint16_t& data)
{
    m_CurrentNode->SetUint(data);
}

template<>
inline void YAMLWrite::TransferBasicData<uint8_t>(uint8_t& data)
{
    if (!m_MetaFlags.empty() && (m_MetaFlags.back() & kTreatIntegerValueAsBoolean))
    {
        m_CurrentNode->SetBool(data != 0);
    }
    else
    {
        m_CurrentNode->SetUint(data);
    }
}

template<>
inline void YAMLWrite::TransferBasicData<int64_t>(int64_t& data)
{
    if (!m_MetaFlags.empty() && (m_MetaFlags.back() & kTransferHex64))
    {
        char valueStr[sizeof(int64_t) * 2 + 1];
        BytesToHexString(&data, sizeof(int64_t), valueStr);
        valueStr[sizeof(int64_t) * 2] = '\0';
        TransferStringToCurrentNode(valueStr);
    }
    else
    {
        m_CurrentNode->SetUint64(data);
    }
}

template<>
inline void YAMLWrite::TransferBasicData<uint64_t>(uint64_t& data)
{
    if (!m_MetaFlags.empty() && (m_MetaFlags.back() & kTransferHex64))
    {
        char valueStr[sizeof(uint64_t) * 2 + 1];
        BytesToHexString(&data, sizeof(uint64_t), valueStr);
        valueStr[sizeof(uint64_t) * 2] = '\0';
        TransferStringToCurrentNode(valueStr);
    }
    else
    {
        m_CurrentNode->SetUint64(data);
    }
}

template<typename T>
void YAMLWrite::Transfer(T& data, const char* _name, TransferMetaFlags metaFlag)
{
    Value* parent = m_CurrentNode;

    Value child;
    child.SetObject();
    m_CurrentNode = &child;
    m_MetaFlags.push_back(metaFlag);

    YAMLSerializeTraits<T>::Transfer(data, *this);

    m_MetaFlags.pop_back();

    if (m_CurrentNode != NULL)
    {
        AppendToNode(parent, _name, child);
    }

    m_CurrentNode = parent;
}

template<typename T>
void YAMLWrite::TransferArray(T& data, TransferMetaFlags metaFlag)
{
    m_CurrentNode->SetArray();

    typename T::iterator i = data.begin();
    typename T::iterator end = data.end();
    while (i != end)
    {
        Transfer(*i, "data", metaFlag);
        ++i;
    }
}

template<typename T>
void YAMLWrite::TransferMap(T& data, TransferMetaFlags metaFlag)
{
    m_CurrentNode->SetArray();

    typename T::iterator i = data.begin();
    typename T::iterator end = data.end();

    Value* parentNode = m_CurrentNode;

    typedef typename NonConstContainerValueType<T>::value_type non_const_value_type;

    while (i != end)
    {
        non_const_value_type& p = reinterpret_cast<non_const_value_type&>(*i);
        Value pairNode;
        pairNode.SetObject();
        m_CurrentNode = &pairNode;

        TransferPair(p, metaFlag);

        parentNode->PushBack(pairNode, m_Allocator);
        i++;
    }

    m_CurrentNode = parentNode;
}

template<typename T>
void YAMLWrite::TransferPair(T& data, TransferMetaFlags)
{
    typedef typename T::first_type first_type;
    typedef typename T::second_type second_type;

    Value* parent = m_CurrentNode;

    Value firstNode;
    firstNode.SetObject();
    m_CurrentNode = &firstNode;
    YAMLSerializeTraits<first_type>::Transfer(data.first, *this);

    Value secondNode;
    secondNode.SetObject();
    m_CurrentNode = &secondNode;
    YAMLSerializeTraits<second_type>::Transfer(data.second, *this);

    parent->AddMember("first", firstNode, m_Document.GetAllocator());
    parent->AddMember("second", secondNode, m_Document.GetAllocator());

    m_CurrentNode = parent;
}
