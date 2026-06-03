#include "GameObject.h"

#include "ObjectDefines.h"
#include "Runtime/Application/Application.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/Transform/TransformComponent.h"
#include "Runtime/Resource/Asset/AssetManager.h"

#include <cassert>
#include <unordered_set>

IMPLEMENT_OBJECT_SERIALIZE(GameObject);
IMPLEMENT_REGISTER_CLASS(GameObject);

bool shouldComponentTick(std::string component_type_name)
{
    if (g_isEditorMode)
    {
        return g_editorTickComponentTypes.find(component_type_name) != g_editorTickComponentTypes.end();
    }
    else
    {
        return true;
    }
}

GameObject::~GameObject()
{
    for (auto& component : m_Components)
    {
    }
    m_Components.clear();
}

void GameObject::Tick(float delta_time)
{
    for (auto& component : m_Components)
    {
        if (component == nullptr || !component->isEnabled())
        {
            continue;
        }

        if (!shouldComponentTick(component->GetTypeName()))
        {
            continue;
        }

        component->Tick(delta_time);
    }
}

bool GameObject::HasComponent(const std::string& compenent_type_name) const
{
    for (const auto& component : m_Components)
    {
    }

    return false;
}

bool GameObject::load(const GameObject& object_instance_res)
{
    // clear old components
    m_Components.clear();
    m_Component.clear();

    SetName(object_instance_res.name);

    // load object instanced components
    m_Components = object_instance_res.m_Components;
    for (auto component : m_Components)
    {
        if (component)
        {
            m_Component.emplace_back(component);
            component->PostLoadResource(this);
        }
    }

    // load object definition components
    m_DefinitionUrl = object_instance_res.m_DefinitionUrl;

    return true;
}

void GameObject::save(GameObject& out_object_instance_res)
{
    out_object_instance_res.name = name;
    out_object_instance_res.m_DefinitionUrl = m_DefinitionUrl;
    out_object_instance_res.m_Components = m_Components;
    out_object_instance_res.m_Component.clear();

    for (auto component : m_Components)
    {
        if (component)
        {
            out_object_instance_res.m_Component.emplace_back(component);
        }
    }
}

template<typename TransferFunction>
void GameObject::Transfer(TransferFunction& transfer)
{
    // Persist identity so a serialized GameObject is self-describing on read.
    // `name` is Object::name; both are new fields, so old binary prefabs that
    // predate them read back as defaults (SafeBinaryRead skips missing fields).
    transfer.Transfer(name, "m_Name");
    transfer.Transfer(m_DefinitionUrl, "m_DefinitionUrl");
    TransferComponents(transfer);
}

void GameObject::SyncSerializedComponents()
{
    // m_Component (the serialized ImmediatePtr container) <- m_Components (the
    // live runtime list). Call right before writing so each ImmediatePtr points
    // at the live Component, which the resolver then encodes as a local fileID.
    m_Component.clear();
    for (auto& component_ptr : m_Components)
    {
        Component* component = component_ptr;
        if (component != nullptr)
        {
            m_Component.emplace_back(component);
        }
    }
}

void GameObject::RebuildRuntimeComponents()
{
    // m_Components (live runtime list) <- m_Component (just-deserialized
    // ImmediatePtr container). Call after reading so getComponents() / tryGet
    // work and Level::CreateObject can copy + PostLoadResource them.
    m_Components.clear();
    for (const auto& pair : m_Component)
    {
        Component* component = pair.GetComponentPtr();
        if (component != nullptr)
        {
            m_Components.emplace_back(component);
        }
    }
}

template<typename TransferFunction>
void GameObject::TransferComponents(TransferFunction& transfer)
{
    transfer.Transfer(m_Component, "m_Component", TransferMetaFlags::HideInEditorMask | TransferMetaFlags::StrongPPtrMask | TransferMetaFlags::DisallowSerializedPropertyModification);
}

template<typename TransferFunction>
void GameObject::ComponentPair::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(component, "component");
}

Component* GameObject::QueryComponentByType(const Type* type) const
{
    Container::const_iterator i;
    Container::const_iterator end = m_Component.end();
    for (i = m_Component.begin(); i != end; ++i)
    {
        if (type->IsBaseOf(i->GetTypeIndex()))
            return i->GetComponentPtr();
    }
    return nullptr;
}