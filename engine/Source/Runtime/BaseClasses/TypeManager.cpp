#include "TypeManager.h"

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/ObjectDefines.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Type.h"

#include <cstring>
#include <mutex>
#include <vector>

#define KERNEL_BUILTIN(x) \
    RegisterNonObjectType(typeid(x).hash_code(), &TypeContainer<x>::m_Type, #x, "");

void TypeManager::Initialize()
{
    Reset();

    TypeRegistrationDesc desc;
    memset(&desc, 0, sizeof(TypeRegistrationDesc));

    desc.init.className = "Object";
    desc.init.classNamespace = "";
    desc.init.assetTypeID = 0;
    desc.init.size = sizeof(Object);
    desc.init.derivedFromInfo.typeIndex = Type::DefaultTypeIndex;
    desc.init.derivedFromInfo.descendantCount = Type::DefaultDescendentCount;
    desc.init.isAbstract = true;
    desc.type = &TypeContainer<Object>::m_Type;

    RegisterType(desc);
    KERNEL_BUILTIN(int);
    KERNEL_BUILTIN(bool);
    KERNEL_BUILTIN(float);
    KERNEL_BUILTIN(void);
    KERNEL_BUILTIN(Vector3);
    InitializeAllTypes();
}

void TypeManager::Reset()
{
    for (auto& i : m_TypeMap)
    {
        if (i.second != nullptr)
        {
            i.second->derivedFromInfo.typeIndex = Type::DefaultTypeIndex;
            i.second->derivedFromInfo.descendantCount = Type::DefaultDescendentCount;
        }
    }

    m_TypeMap.clear();
    m_KlassNameToType.clear();
    m_ClassNameStringToType.clear();
    m_RuntimeTypes.count = 0;
    memset(m_RuntimeTypes.types, 0, sizeof(m_RuntimeTypes.types));

    AttributeMapEntry* entry = AttributeMapEntry::s_Head;
    while (entry != nullptr)
    {
        entry->types.reset();
        entry = entry->next;
    }
}

void TypeManager::InitializeAllTypes()
{
    m_RuntimeTypes.count = 0;
    memset(m_RuntimeTypes.types, 0, sizeof(m_RuntimeTypes.types));

    AttributeLookupMap attributeLookupMap = CreateAttributeLookupMap();
    AttributeMapEntry* attributeEntry = AttributeMapEntry::s_Head;
    while (attributeEntry != nullptr)
    {
        attributeEntry->types.reset();
        attributeEntry = attributeEntry->next;
    }

    for (auto& i : m_TypeMap)
    {
        if (i.second != nullptr)
        {
            i.second->derivedFromInfo.typeIndex = Type::DefaultTypeIndex;
            i.second->derivedFromInfo.descendantCount = Type::DefaultDescendentCount;
        }
    }

    Builder builder;
    m_RuntimeTypes.count = builder.Build(m_TypeMap);

    for (RTTIMap::iterator i = m_TypeMap.begin(); i != m_TypeMap.end(); ++i)
    {
        if (i->second == nullptr)
        {
            continue;
        }

        auto& rttiInfo = i->second->derivedFromInfo;
        if (rttiInfo.typeIndex < RuntimeTypeArray::MAX_RUNTIME_TYPES)
        {
            m_RuntimeTypes.types[rttiInfo.typeIndex] = i->second;
            RegisterTypeInGlobalAttributeMap(*i->second, attributeLookupMap);
        }
    }
}

const Type* TypeManager::ClassNameToType(const char* name) const
{
    if (name == nullptr)
        return nullptr;

    // Content-based lookup (works for parsed / runtime-built strings). The
    // pointer-keyed m_KlassNameToType is kept for the rare same-literal callers
    // but a string compare is what the by-name deserialization paths need.
    auto&& s = m_ClassNameStringToType.find(name);
    if (s != m_ClassNameStringToType.end())
        return s->second;

    auto&& i = m_KlassNameToType.find(name);
    if (i != m_KlassNameToType.end())
        return i->second;
    return nullptr;
}

void TypeManager::RegisterType(const TypeRegistrationDesc& desc)
{
    if (desc.type == nullptr)
    {
        return;
    }

    Type& destination = *desc.type;
    destination = desc.init;

    m_TypeMap[destination.assetTypeID] = &destination;
    m_KlassNameToType[destination.className] = &destination;
    if (destination.className != nullptr)
        m_ClassNameStringToType[destination.className] = &destination;
}

void TypeManager::RegisterClassNameAlias(const char* alias, const Type* type)
{
    if (alias == nullptr || alias[0] == '\0' || type == nullptr)
    {
        return;
    }
    m_ClassNameStringToType[alias] = type;
}

void TypeManager::FindAllDerivedTypes(const Type* baseType, std::vector<const Type*>& derivedTypes, bool onlyNonAbstrace) const
{
    const uint32_t typeIndexBegin = baseType->derivedFromInfo.typeIndex;
    const uint32_t typeIndexEnd = baseType->derivedFromInfo.typeIndex + baseType->derivedFromInfo.descendantCount;

    derivedTypes.reserve(baseType->derivedFromInfo.descendantCount);
    if (onlyNonAbstrace)
    {
        for (uint32_t typeIndex = typeIndexBegin; typeIndex < typeIndexEnd; ++typeIndex)
        {
            const Type* type = m_RuntimeTypes.types[typeIndex];
            if (!type->isAbstract)
                derivedTypes.push_back(type);
        }
    }
}

TypeManager::AttributeLookupMap TypeManager::CreateAttributeLookupMap()
{
    AttributeLookupMap attrLookupMap;
    AttributeMapEntry* entry = AttributeMapEntry::s_Head;
    while (entry != nullptr)
    {
        attrLookupMap[entry->attrType] = entry;
        entry = entry->next;
    }
    return attrLookupMap;
}

void TypeManager::RegisterTypeInGlobalAttributeMap(const Type& type, const AttributeLookupMap& attributeLookupMap)
{
    for (size_t attr = 0; attr < type.attributeCount; ++attr)
    {
        AttributeLookupMap::const_iterator it = attributeLookupMap.find(type.attributes[attr].GetType());
        if (it != attributeLookupMap.end() && type.derivedFromInfo.typeIndex < RuntimeTypeArray::MAX_RUNTIME_TYPES)
        {
            it->second->types[type.derivedFromInfo.typeIndex] = true;
        }
    }
}

void TypeManager::RegisterNonObjectType(uint64_t typeID, Type* type, const char* name, const char* nameSpace)
{
    TypeRegistrationDesc desc = TYPE_REGISTRATION_DESC_DEFAULT_INITIALIZER_LIST;
    desc.init.assetTypeID = typeID;
    desc.init.className = name;
    desc.init.classNamespace = nameSpace;
    desc.init.size = 0;
    desc.type = type;
    RegisterType(desc);
}

uint32_t TypeManager::Builder::Build(const std::unordered_map<uint64_t, Type*>& typeMap)
{
    for (auto&& i = typeMap.begin(); i != typeMap.end(); ++i)
    {
        if (i->second != nullptr)
        {
            i->second->derivedFromInfo.typeIndex = Type::DefaultTypeIndex;
            i->second->derivedFromInfo.descendantCount = Type::DefaultDescendentCount;
        }
    }

    LookupOrAdd(&TypeContainer<Object>::m_Type);
    for (auto&& i = typeMap.begin(); i != typeMap.end(); ++i)
    {
        if (i->second != nullptr)
        {
            LookupOrAdd(i->second);
        }
    }

    auto&& nodeCount = m_Nodes.size();
    uint32_t nextTypeIndex = 0;
    for (uint32_t iNode = 0; iNode < nodeCount; ++iNode)
    {
        const Node& node = m_Nodes[iNode];
        if (node.type->derivedFromInfo.typeIndex == Type::DefaultTypeIndex)
            nextTypeIndex += TraversDepthFirst(node, nextTypeIndex);
    }
    return nextTypeIndex;
}

uint32_t TypeManager::Builder::TraversDepthFirst(const Node& node, uint32_t typeIndex)
{
    uint32_t descendantCount = 1;
    for (int32_t child = node.firstChild; child != -1; child = m_Nodes[child].nextSibling)
        descendantCount += TraversDepthFirst(m_Nodes[child], typeIndex + descendantCount);

    Type::DrivedFromInfo& derivedFromInfo = node.type->derivedFromInfo;
    derivedFromInfo.typeIndex = typeIndex;
    derivedFromInfo.descendantCount = descendantCount;
    return descendantCount;
}

int32_t TypeManager::Builder::Add(Type* type)
{
    Type* baseType = const_cast<Type*>(type->base);
    int32_t baseID = baseType ? LookupOrAdd(baseType) : -1;
    int32_t newNodeID = m_Nodes.size();
    Node& newNode = m_Nodes.emplace_back();
    newNode.type = type;
    newNode.firstChild = -1;

    type->derivedFromInfo.typeIndex = newNodeID;

    if (baseType == nullptr)
    {
        newNode.nextSibling = -1;
    }
    else
    {
        int32_t* prevNodeNext = &m_Nodes[baseID].firstChild;
        while ((*prevNodeNext != -1) && (strcmp(m_Nodes[*prevNodeNext].type->className, type->className) < 0))
        {
            prevNodeNext = &m_Nodes[*prevNodeNext].nextSibling;
        }
        newNode.nextSibling = *prevNodeNext;
        *prevNodeNext = newNodeID;
    }
    return newNodeID;
}

int32_t TypeManager::Builder::LookupOrAdd(Type* type)
{
    int32_t newNodeId = type->derivedFromInfo.typeIndex;
    if (newNodeId == Type::DefaultTypeIndex)
        newNodeId = Add(type);
    return newNodeId;
}

void GlobalRegisterType(const TypeRegistrationDesc& desc)
{
    TypeManager::GetInstance().RegisterType(desc);
}

// 自动注册机制实现
namespace AutoTypeRegistration
{
    std::vector<RegisterFunction>& GetRegisterFunctions()
    {
        static std::vector<RegisterFunction> s_Registerfunctions;
        return s_Registerfunctions;
    }

    void AddRegisterFunction(RegisterFunction func)
    {
        static std::mutex s_Mutex;
        std::lock_guard<std::mutex> lock(s_Mutex);
        GetRegisterFunctions().push_back(std::move(func));
    }

    void ExecuteAllRegistrations()
    {
        auto& functions = GetRegisterFunctions();
        for (auto& func : functions)
        {
            if (func)
            {
                func();
            }
        }
    }
}  // namespace AutoTypeRegistration
