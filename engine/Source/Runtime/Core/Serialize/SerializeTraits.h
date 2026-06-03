#pragma once

#include "Runtime/Function/CommonStringTable/CommonString.h"
#include "SerializationMetaFlags.h"
#include "SerializeTraitsBase.h"

#define DEFINE_GET_TYPESTRING_BASIC_TYPE(x)                    \
    inline static const char* GetTypeString(void* p = nullptr) \
    {                                                          \
        return COMMON_STRING(x);                               \
    }                                                          \
    inline static bool AllowTransferOptimization()             \
    {                                                          \
        return true;                                           \
    }

template<>
struct SerializeTraits<int32_t> : public SerializeTraitsBaseForBasicType<int32_t>
{
    DEFINE_GET_TYPESTRING_BASIC_TYPE(int)
};

template<>
struct SerializeTraits<uint32_t> : public SerializeTraitsBaseForBasicType<uint32_t>
{
    DEFINE_GET_TYPESTRING_BASIC_TYPE(uint32_t)
};

template<>
struct SerializeTraits<int64_t> : public SerializeTraitsBaseForBasicType<int64_t>
{
    DEFINE_GET_TYPESTRING_BASIC_TYPE(int64_t)
};

template<>
struct SerializeTraits<uint64_t> : public SerializeTraitsBaseForBasicType<uint64_t>
{
    DEFINE_GET_TYPESTRING_BASIC_TYPE(uint64_t)
};

// 8-bit / 16-bit integer specialisations. Required so that
// SerializeTraits<typename T::value_type>::AllowTransferOptimization() in
// TransferArray (StreamedBinaryRead/Write) does not fall through to the
// generic SerializeTraits<T> primary template -- which assumes T is a class
// with a static AllowTransferOptimization() member and triggers
// C2825 "T must be a class or namespace when followed by '::'".
//
// Concretely: Texture2D::m_Pixels is std::vector<uint8_t>, so without these
// specialisations the binary blob path for byte buffers cannot compile.
template<>
struct SerializeTraits<int8_t> : public SerializeTraitsBaseForBasicType<int8_t>
{
    DEFINE_GET_TYPESTRING_BASIC_TYPE(SInt8)
};

template<>
struct SerializeTraits<uint8_t> : public SerializeTraitsBaseForBasicType<uint8_t>
{
    DEFINE_GET_TYPESTRING_BASIC_TYPE(UInt8)
};

template<>
struct SerializeTraits<int16_t> : public SerializeTraitsBaseForBasicType<int16_t>
{
    DEFINE_GET_TYPESTRING_BASIC_TYPE(SInt16)
};

template<>
struct SerializeTraits<uint16_t> : public SerializeTraitsBaseForBasicType<uint16_t>
{
    DEFINE_GET_TYPESTRING_BASIC_TYPE(UInt16)
};

template<>
struct SerializeTraits<char> : public SerializeTraitsBaseForBasicType<char>
{
    DEFINE_GET_TYPESTRING_BASIC_TYPE(char);
};

template<>
struct SerializeTraits<float> : public SerializeTraitsBaseForBasicType<float>
{
    DEFINE_GET_TYPESTRING_BASIC_TYPE(float);
};

template<>
struct SerializeTraits<double> : public SerializeTraitsBaseForBasicType<double>
{
    DEFINE_GET_TYPESTRING_BASIC_TYPE(double);
};

template<>
struct SerializeTraits<bool> : public SerializeTraitsBaseForBasicType<bool>
{
    using value_type = bool;
    DEFINE_GET_TYPESTRING_BASIC_TYPE(bool);

    template<typename TransferFunction>
    inline static void Transfer(value_type& data, TransferFunction& transfer)
    {
        transfer.TransferBasicData(data);
    }
};

template<>
class SerializeTraits<eastl::string> : public SerializeTraitsBase<eastl::string>
{
public:
    using value_type = eastl::string;
    inline static const char* GetTypeString(value_type* p = nullptr) { return COMMON_STRING(string); }

    inline static bool AllowTransferOptimization() { return false; }
    static bool IsContinousMemoryArray() { return true; }

    static void ResizeArray(value_type& data, int rs) { data.resize(rs); }

    template<typename TransferFunction>
    inline static void Transfer(value_type& data, TransferFunction& transfer)
    {
        transfer.TransferStringData(data);
        transfer.Align();
    }
};

template<typename T, typename Allocator>
class SerializeTraits<std::vector<T, Allocator>> : public SerializeTraitsBase<std::vector<T, Allocator>>
{
public:
    using value_type = std::vector<T, Allocator>;
    DEFINE_GET_TYPESTRING_BASIC_TYPE(vector)

    template<typename TransferFunction>
    inline static void Transfer(value_type& data, TransferFunction& transfer)
    {
        transfer.TransferArray(data);
    }

    static bool IsContinousMemoryArray() { return true; }
    static void ResizeArray(value_type& data, int rs) { data.resize(rs); }
};

template<typename Key, typename T>
class SerializeTraits<std::pair<Key, T>> : public SerializeTraitsBase<std::pair<Key, T>>
{
public:
    using value_type = std::pair<Key, T>;
    inline static const char* GetTypeString(void* x = nullptr) { return COMMON_STRING(pair); }
    inline static bool AllowTransferOptimization() { return false; }

    template<typename TransferFunction>
    inline static void Transfer(value_type& data, TransferFunction& transfer)
    {
        transfer.Transfer(data.first, COMMON_STRING(first));
        transfer.Transfer(data.second, COMMON_STRING(second));
    }
};

template<typename Key, typename T, class Hash, class KeyEqual, class Allocator>
class SerializeTraits<std::unordered_map<Key, T, Hash, KeyEqual, Allocator>> : public SerializeTraitsBase<std::unordered_map<Key, T, Hash, KeyEqual, Allocator>>
{
public:
    using value_type = std::unordered_map<Key, T, Hash, KeyEqual, Allocator>;
    DEFINE_GET_TYPESTRING_BASIC_TYPE(map)

    template<typename TransferFunction>
    inline static void Transfer(value_type& data, TransferFunction& transfer)
    {
        transfer.TransferMap(data);
    }
};

template<typename T>
struct NonConstContainerValueType
{
    using value_type = typename T::value_type;
};

template<typename Key, typename T, class Hash, class KeyEqual, class Allocator>
struct NonConstContainerValueType<std::unordered_map<Key, T, Hash, KeyEqual, Allocator>>
{
    typedef std::pair<Key, T> value_type;
};