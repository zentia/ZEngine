#pragma once
#include "Runtime/BaseClasses/ImmediatePtr.h"
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
class GameObject;
// Component
class Component : public Object
{
    REGISTER_CLASS(Component);
    DECLARE_OBJECT_SERIALIZE();

protected:
    GameObject* m_ParentObject;
    bool m_IsDirty {false};
    bool m_IsScaleDirty {false};

    bool m_Enabled {true};

public:
    Component() = default;
    virtual ~Component() = default;

    // Instantiating the component after definition loaded
    virtual void PostLoadResource(GameObject* parent_object) { m_ParentObject = parent_object; }

    /// Read-only access to the GameObject this Component is attached to. Used by
    /// editor code (e.g. PrefabUtilityEditor's component-suppress pipeline)
    /// that needs to navigate from a Component back to its host without
    /// re-walking every GameObject's m_Components list.
    GameObject* GetParentObject() const { return m_ParentObject; }

    virtual void Tick(float delta_time) {};
    virtual void OnSerializedFieldsUpdated() {}

    bool IsDirty() const { return m_IsDirty; }

    void setDirtyFlag(bool is_dirty) { m_IsDirty = is_dirty; }
    virtual bool isEnabled() const { return m_Enabled; }
    virtual void SetEnabled(bool enabled) { m_Enabled = enabled; }

    bool m_TickInEditorMode {false};
};

class ComponentDefinitionRes
{
public:
    std::string m_TypeName;
    std::string m_Component;
};

struct ObjectInstanceRes
{
    DECLARE_SERIALIZE(ObjectInstanceRes)

public:
    eastl::string m_Name;
    eastl::string m_Definition;

    std::vector<ImmediatePtr<Component>> m_InstancedComponents;
};

template<typename TransferFunction>
void ObjectInstanceRes::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_Name, "name");
    transfer.Transfer(m_Definition, "definition");
    transfer.Transfer(m_InstancedComponents, "instanced_components");
}