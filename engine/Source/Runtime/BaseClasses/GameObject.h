#pragma once

#include "ImmediatePtr.h"
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Serialize/TransferFunctions/SerializeTransfer.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"

#include <memory>
#include <string>
#include <unordered_set>

class GameObject : public Object
{
    REGISTER_CLASS(GameObject);
    DECLARE_OBJECT_SERIALIZE();
    typedef std::unordered_set<std::string> TypeNameSet;

public:
    struct ComponentPair
    {
        ComponentPair()
            : typeIndex(Type::DefaultTypeIndex), component(nullptr)
        {
        }
        ComponentPair(Component* in_component)
            : typeIndex((in_component != nullptr) ? in_component->GetType()->GetRuntimeTypeIndex() : Type::DefaultTypeIndex),
              component(in_component)
        {
        }
        DECLARE_SERIALIZE(ComponentPair);
        inline uint32_t const GetTypeIndex() const { return typeIndex; }
        inline ImmediatePtr<Component> const& GetComponentPtr() const { return component; }

    private:
        uint32_t typeIndex;
        ImmediatePtr<Component> component;
    };

    typedef std::vector<ComponentPair> Container;
    GameObject()
        : m_Id(0)
    {
        // Level::CreateObject uses make_shared<GameObject>; unlike components
        // created via MemoryManager::CreateObject, we must seed m_CachedTypeIndex
        // here so YAML scene save can call GetTypeName() on live GameObjects.
        InitializeRuntimeTypeInfo();
    }
    GameObject(GObjectID id)
        : m_Id(id)
    {
        InitializeRuntimeTypeInfo();
    }
    virtual ~GameObject();

    virtual void Tick(float delta_time);

    bool load(const GameObject& object_instance_res);
    void save(GameObject& out_object_instance_res);

    // Bridge between the live runtime component list (m_Components) and the
    // serialized ImmediatePtr container (m_Component). SyncSerializedComponents
    // is called before a graph write; RebuildRuntimeComponents after a graph
    // read. See GameObject.cpp for the rationale behind the two-container split.
    void SyncSerializedComponents();
    void RebuildRuntimeComponents();

    GObjectID GetID() const { return m_Id; }

    void SetName(eastl::string name) { this->name = name; }
    const eastl::string& GetName() const { return name; }
    const eastl::string& getDefinitionUrl() const { return m_DefinitionUrl; }

    bool HasComponent(const std::string& compenent_type_name) const;

    void addComponent(Component* component)
    {
        if (component != nullptr)
        {
            m_Components.emplace_back(component);
        }
    }

    std::vector<ImmediatePtr<Component>> getComponents() { return m_Components; }

    // Editor-only mutable accessor + targeted remove. PrefabUtilityEditor's
    // Add/Remove/Suppress/Unsuppress component pipeline needs to splice the
    // m_Components list in-place (the value-returning `getComponents()` makes
    // a copy, which is unusable for editing). We expose this as a separate
    // `Editor*` named API so runtime code can keep treating the list as
    // read-only via `getComponents()`.
    std::vector<ImmediatePtr<Component>>& EditorGetComponentsRef() { return m_Components; }

    /// Remove the first ImmediatePtr in m_Components whose target equals
    /// `component`. Returns true if a slot was actually removed. Does NOT
    /// destroy the Component object — ownership/lifetime stays with the
    /// SerializedFile / ObjectManager.
    bool EditorRemoveComponent(Component* component)
    {
        if (component == nullptr)
            return false;
        for (auto it = m_Components.begin(); it != m_Components.end(); ++it)
        {
            if (static_cast<Component*>(*it) == component)
            {
                m_Components.erase(it);
                return true;
            }
        }
        return false;
    }

    template<typename TComponent>
    TComponent* tryGetComponent(const std::string& compenent_type_name)
    {
        (void)compenent_type_name;
        for (auto& component : m_Components)
        {
            TComponent* typed_component = dynamic_cast<TComponent*>(static_cast<Component*>(component));
            if (typed_component != nullptr)
            {
                return typed_component;
            }
        }

        return nullptr;
    }

    template<typename TComponent>
    const TComponent* tryGetComponentConst(const std::string& compenent_type_name) const
    {
        (void)compenent_type_name;
        for (const auto& component : m_Components)
        {
            const TComponent* typed_component = dynamic_cast<const TComponent*>(static_cast<Component*>(component));
            if (typed_component != nullptr)
            {
                return typed_component;
            }
        }
        return nullptr;
    }

    Component* QueryComponentByType(const Type* type) const;

#define tryGetComponent(COMPONENT_TYPE)      tryGetComponent<COMPONENT_TYPE>(#COMPONENT_TYPE)
#define tryGetComponentConst(COMPONENT_TYPE) tryGetComponentConst<const COMPONENT_TYPE>(#COMPONENT_TYPE)

protected:
    GObjectID m_Id {k_invalid_gobject_id};
    eastl::string m_DefinitionUrl;

    // we have to use the ReflectionPtr due to that the components need to be reflected
    // in editor, and it's polymorphism
    std::vector<ImmediatePtr<Component>> m_Components;

private:
    template<typename TransferFunction>
    void TransferComponents(TransferFunction& transfer);

    Container m_Component;
};
