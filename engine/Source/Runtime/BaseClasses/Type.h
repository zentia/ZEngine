#pragma once

#include "RuntimeTypeArray.h"
#include "Variant.h"

#include <bitset>
#include <stdint.h>

class Object;

enum TypeFlags
{
    kTypeNoFlags = 0,
    kTypeIsAbstract = 1 << 0,
    kTypeIsSealed = 1 << 1,
    kTypeIsEditorOnly = 1 << 2,
};

using TypeBitset = std::bitset<RuntimeTypeArray::MAX_RUNTIME_TYPES>;

class Type
{
public:
    enum
    {
        UndefinedPersistentTypeID = 0
    };

    enum
    {
        DefaultTypeIndex = 0x80000000,
        DefaultDescendentCount = 0,
    };

    enum TypeFilterOptions
    {
        kAllTypes,
        kOnlyNonAbstract,
    };

    class DrivedFromInfo
    {
    public:
        uint32_t typeIndex;
        uint32_t descendantCount;
    };

    inline bool IsBaseOf(uint32_t typeIndex) const
    {
        return (typeIndex - derivedFromInfo.typeIndex) < derivedFromInfo.descendantCount;
    }

    uint32_t GetRuntimeTypeIndex() const { return derivedFromInfo.typeIndex; }
    using FactoryFunction = Object*();
    FactoryFunction* GetFactory() const { return factory; }
    const char* GetName() const { return className; }
    const Type* base;
    FactoryFunction* factory;
    const char* className;
    const char* classNamespace;
    uint64_t assetTypeID;
    int size;
    DrivedFromInfo derivedFromInfo;
    bool isAbstract;
    const ConstVariantRef* attributes;
    size_t attributeCount;
};

template<typename T>
struct TypeContainer
{
    static Type m_Type;
};

#define TYPE_DEFAULT_INITIALIZER_LIST \
    {                                 \
        nullptr, nullptr, "[UNREGISTERED]", "", (uint64_t)Type::UndefinedPersistentTypeID, 0, {(uint32_t)Type::DefaultTypeIndex, (uint32_t)Type::DefaultDescendentCount}, false, nullptr, 0}

template<typename T>
Type TypeContainer<T>::m_Type = TYPE_DEFAULT_INITIALIZER_LIST;

using TypeCallback = void();
struct TypeRegistrationDesc
{
    Type init;
    Type* type;
    TypeCallback* initCallback;
    TypeCallback* postInitCallback;
    TypeCallback* cleanupCallback;
};

#define TYPE_REGISTRATION_DESC_DEFAULT_INITIALIZER_LIST \
    {                                                   \
        TYPE_DEFAULT_INITIALIZER_LIST, NULL, NULL, NULL, NULL}

template<typename T>
const Type* TypeOf()
{
    return reinterpret_cast<Type*>(&TypeContainer<T>::m_Type);
}

struct AttributeMapEntry
{
    static AttributeMapEntry* s_Head;
    TypeBitset types;
    const Type* attrType;
    AttributeMapEntry* next;
};

template<typename TAttribute>
class AttributeMap
{
public:
    AttributeMap()
    {
        m_Entry.next = AttributeMapEntry::s_Head;
        AttributeMapEntry::s_Head = &m_Entry;
        m_Entry.attrType = TypeOf<TAttribute>();
    }

private:
    AttributeMapEntry m_Entry;
};
