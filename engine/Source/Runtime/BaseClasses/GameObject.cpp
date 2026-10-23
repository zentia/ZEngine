#include "GameObject.h"

#include "ObjectDefines.h"
#include "Runtime/Application/Application.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Prefab/PrefabUtility.h"

#include <cassert>
#include <filesystem>
#include <typeinfo>
#include <unordered_set>

IMPLEMENT_OBJECT_SERIALIZE(GameObject);

bool shouldComponentTick(std::string component_type_name)
{
    if (!g_isPlaying)
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
    // Transform::m_Enabled is the GameObject active flag (Unity activeSelf analogue).
    if (Transform* transform_component = tryGetComponent(Transform))
    {
        if (!transform_component->isEnabled())
        {
            return;
        }
    }

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

// Both lookups go through TypeManager::ClassNameToType + Type::IsBaseOf rather
// than `GetTypeName() == name`: the latter reads Object::GetType(), i.e.
// m_CachedTypeIndex, which is only meaningful after InitializeRuntimeTypeInfo()
// has run. GetMostDerivedType() is the virtual lookup and is always valid.
Component* GameObject::GetComponentByTypeName(const std::string& component_type_name) const
{
    const Type* wanted = TypeManager::GetInstance().ClassNameToType(component_type_name.c_str());
    if (wanted == nullptr)
        return nullptr;

    for (const auto& component : m_Components)
    {
        Component* candidate = static_cast<Component*>(component);
        if (candidate == nullptr)
            continue;
        const Type* runtime_type = candidate->GetMostDerivedType();
        if (runtime_type != nullptr && wanted->IsBaseOf(runtime_type->GetRuntimeTypeIndex()))
            return candidate;
    }

    return nullptr;
}

// Was an empty loop that always returned false -- and phase 3 had already bound
// it to TypeScript, so `go.HasComponent("Transform")` was a silent lie.
bool GameObject::HasComponent(const std::string& component_type_name) const
{
    return GetComponentByTypeName(component_type_name) != nullptr;
}

Transform* GameObject::GetTransform() const
{
    return Cast<Transform>(GetComponentByTypeName("Transform"));
}

bool GameObject::load(const GameObject& object_instance_res)
{
    // clear old components
    m_Components.clear();
    m_Component.clear();

    SetName(object_instance_res.name);

    // load object instanced components
    m_Components = object_instance_res.m_Components;

    // Expand the authored definition (a .prefab) before running PostLoadResource,
    // so components that come from the prefab are already attached and see the
    // full component set. See ExpandDefinition for the merge rules.
    m_DefinitionUrl = object_instance_res.m_DefinitionUrl;
    ExpandDefinition();

    for (auto component : m_Components)
    {
        if (component)
        {
            m_Component.emplace_back(component);
            component->PostLoadResource(this);
        }
    }

    return true;
}

void GameObject::ExpandDefinition()
{
    // A scene GameObject is authored as "inline overrides + a prefab definition".
    // The scene inlines only what it overrides (typically the Transform); the
    // prefab carries everything else -- mesh renderer, rigid body, ... Until now
    // nothing expanded this url, so a level loaded as a set of bare transforms
    // with zero colliders.
    if (m_DefinitionUrl.empty())
    {
        return;
    }

    AssetManager* asset_manager = GET_SYSTEM(AssetManager);
    if (asset_manager == nullptr)
    {
        LOG_ERROR(ZObject, "GameObject '{}': cannot expand definition '{}', AssetManager unavailable",
                  name.c_str(), m_DefinitionUrl.c_str());
        return;
    }

    const std::filesystem::path prefab_path = asset_manager->GetFullPath(m_DefinitionUrl);
    // ⚠️ `invoke_post_load = false`，原因见 PrefabUtility::Instantiate 的注释。
    //
    // 这里要的只是 prefab 的**组件对象**，好把它们并进本对象（下面的 merge）。整棵
    // definition 树随后就被丢掉 —— 它不在任何 Level 里、永不 tick、也不会有人析构。
    // 以前这一步照常跑 PostLoadResource，于是模板对象上的组件**先**按模板自己的
    // 变换初始化一遍（粒子发射器在原点开出一份 GPU 资源、拿到一个 emitter id），
    // 然后 load() 再按场景实例的正确变换初始化一遍 ⇒ 场景里 1 个发射器开出 2 份
    // 资源，而模板那一份永远 0 个粒子、每帧照样被 Draw 遍历。
    GameObject* definition_root = PrefabUtility::InstantiateFromPath(prefab_path, /*invoke_post_load=*/false);
    if (definition_root == nullptr)
    {
        LOG_ERROR(ZObject, "GameObject '{}': failed to instantiate definition '{}'",
                  name.c_str(), m_DefinitionUrl.c_str());
        return;
    }

    // Merge by type, inline-wins: a component already present on this object is
    // an override and is never replaced. This keeps the scene's authored
    // NavMeshVolumeComponent and its placement Transform, while adding everything
    // the prefab contributes.
    //
    // The check runs against m_Components (the live list), not m_Component (the
    // deserialized container): at this point in load() the latter is still empty.
    // dynamic_cast also catches subclass overrides, which a name compare misses.
    for (ImmediatePtr<Component>& definition_component : definition_root->getComponents())
    {
        Component* candidate = definition_component;
        if (candidate == nullptr)
        {
            continue;
        }

        bool is_already_present = false;
        for (const ImmediatePtr<Component>& existing : m_Components)
        {
            Component* existing_component = existing;
            if (existing_component != nullptr && typeid(*existing_component) == typeid(*candidate))
            {
                is_already_present = true;
                break;
            }
        }
        if (is_already_present)
        {
            continue;
        }
        m_Components.emplace_back(candidate);
    }

    LOG_INFO(ZObject, "GameObject '{}': expanded definition '{}' to {} component(s)",
             name.c_str(), m_DefinitionUrl.c_str(), m_Components.size());
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