#pragma once
#include "Runtime/BaseClasses/ImmediatePtr.h"
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Component.generated.h"
class GameObject;
// Component
ZCLASS()
class Component : public Object
{
    ZGENERATED_BODY(Component)

protected:
    // 必须初始化：组件 New 出来到 PostLoadResource 挂上父对象之间，SetScriptGuid 之类的 setter 用
    // `GetParentObject() != nullptr` 判断"是否已挂上"；未初始化时那是堆上的垃圾（spine.spawn 带 script=
    // 时在 TypeScriptComponent::BindAndAwake → GetTransform 里崩过，取决于堆布局，时有时无）
    GameObject* m_ParentObject {nullptr};
    bool m_IsDirty {false};
    bool m_IsScaleDirty {false};

    ZPROPERTY()
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
    ///
    /// ZFUNCTION: also how the TS Behaviour binding publishes
    /// `this.component.gameObject`. Binding `Component` at all is what makes the
    /// pesapi registry know the type id, which `native_object_to_value` needs
    /// before any Component* can be handed to JS.
    ZFUNCTION()
    GameObject* GetParentObject() const { return m_ParentObject; }

    /// Bind the owner back-pointer WITHOUT running PostLoadResource. For template
    /// graphs -- a PrefabAsset loaded to be cloned, applied to or re-saved -- that must
    /// be navigable child Transform -> GameObject but must not register with live
    /// systems (scene roots, renderers, physics, scripts). Never overwrites an owner
    /// that is already bound.
    void BindOwnerIfUnbound(GameObject* owner)
    {
        if (m_ParentObject == nullptr)
        {
            m_ParentObject = owner;
        }
    }

    virtual void Tick(float delta_time) {};
    virtual void OnSerializedFieldsUpdated() {}

    /// 宿主 GameObject 的**层级激活状态**（activeInHierarchy）刚发生变化时的通知。
    /// 传进来的 `active` 是变化后的新值：父级被停用时，子对象自己的 activeSelf
    /// 没动，但这里收到 false。
    ///
    /// 为什么必须有这个钩子、不能让组件自己在 Tick 里判断：GameObject::Tick 在对象
    /// 未激活时**直接 return**，组件压根不会被 Tick 到。也就是说"停用"这件事在
    /// 时序上没有任何一次 Tick 能替组件收尾 —— BaseRenderer 那条"提交空 parts 撤掉
    /// 渲染实体"的路子就一直是死的（对象停用了，画面上还留着）。状态变化只能由
    /// GameObject 主动推给组件。
    ///
    /// 语义对应 Unity 的 OnEnable / OnDisable，但**不**排队到帧末：这里要的就是
    /// 立刻生效（同一帧内画面就得跟着变）。
    virtual void OnOwnerActiveChanged(bool active) { (void)active; }

    /// Editor undo/redo just overwrote this component's serialized fields in place
    /// (UE UObject::PostEditUndo). The default re-derives runtime state the same way
    /// an Inspector edit does; override when a field change needs more than that --
    /// e.g. a path that is only resolved in PostLoadResource.
    virtual void PostEditUndo() { OnSerializedFieldsUpdated(); }

    bool IsDirty() const { return m_IsDirty; }

    void setDirtyFlag(bool is_dirty) { m_IsDirty = is_dirty; }
    ZFUNCTION()
    virtual bool isEnabled() const { return m_Enabled; }
    ZFUNCTION()
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