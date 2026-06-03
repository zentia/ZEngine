#pragma once
#include "Runtime/Core/Base/Singleton.h"
#include "RuntimeTypeArray.h"
#include "Type.h"

#include <string>
#include <unordered_map>

struct TypeRegistrationDesc;

class TypeManager : public Singleton<TypeManager>
{
public:
    void Initialize();
    void InitializeAllTypes();
    const Type* ClassNameToType(const char* name) const;

    template<typename T>
    void RegisterType(int32_t assetTypeID, const char* name, const char* nameSpace)
    {
        TypeRegistrationDesc desc = TYPE_REGISTRATION_DESC_DEFAULT_INITIALIZER_LIST;
        desc.type = &TypeContainer<T>::type;
        desc.init.assetTypeID = assetTypeID;
        desc.init.className = name;
        desc.init.classNamespace = nameSpace;
        desc.init.attributes = 0;
        RegisterType(desc);
    }

    void RegisterType(const TypeRegistrationDesc& desc);

    /// Map a legacy serialized class name (e.g. "MaterialRes") to an existing Type.
    void RegisterClassNameAlias(const char* alias, const Type* type);

    RuntimeTypeArray& GetRuntimeTypes() { return m_RuntimeTypes; }
    void FindAllDerivedTypes(const Type* baseType, std::vector<const Type*>& derivedTypes, bool onlyNonAbstrace) const;
    void RegisterNonObjectType(uint64_t typeID, Type* type, const char* name, const char* nameSpace);
    using AttributeLookupMap = std::unordered_map<const Type*, AttributeMapEntry*>;
    static AttributeLookupMap CreateAttributeLookupMap();
    static void RegisterTypeInGlobalAttributeMap(const Type& type, const AttributeLookupMap& attributeLookupMap);

private:
    void Reset();

    class Builder
    {
        struct Node
        {
            Type* type;
            int32_t firstChild;
            int32_t nextSibling;
        };

    public:
        uint32_t Build(const std::unordered_map<uint64_t, Type*>& typeMap);
        uint32_t TraversDepthFirst(const Node& node, uint32_t typeIndex);

    private:
        int32_t Add(Type* type);
        int32_t LookupOrAdd(Type* type);
        std::vector<Node> m_Nodes;
    };
    std::unordered_map<const char*, const Type*> m_KlassNameToType;
    // Content-keyed mirror of m_KlassNameToType. m_KlassNameToType keys on the
    // raw className pointer (std::hash<const char*> hashes the address, not the
    // chars), so it only ever matches when the caller passes the SAME literal
    // that REGISTER_CLASS stored. Deserialization-by-name (YamlObjectGraph,
    // asset-type label, DataTable inspector) hands in freshly parsed buffers
    // whose pointer never matches, so ClassNameToType resolves through this
    // std::string-keyed map instead.
    std::unordered_map<std::string, const Type*> m_ClassNameStringToType;
    using RTTIMap = std::unordered_map<uint64_t, Type*>;
    RTTIMap m_TypeMap;
    RuntimeTypeArray m_RuntimeTypes;
};