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

    /// 每帧一次，`delta_time` 是**帧间隔**（受 timeScale，但随帧率浮动）。
    virtual void Tick(float delta_time) {};

    /// 每个固定步一次，`fixed_delta_time` 恒为 `TimeState::GetFixedDeltaTime()`。
    ///
    /// 与 Tick 的分工
    /// --------------
    /// 一帧内 FixedTick 可能跑 **0 次**（帧率高于固定步率）也可能跑多次
    /// （掉帧补步），且**一定在 Tick 之前**（见 Application::LogicalTick）。
    /// 所以：
    ///   - 需要确定性/可复现的东西放这里 —— 物理积分、角色移动、状态机计时、
    ///     联网需要对齐的状态推进。
    ///   - 只影响表现的东西放 Tick —— 输入采样后的一次性响应、UI 动画、相机
    ///     跟随（除非你要它跟物理同步）。
    /// 两边都写同一个状态是典型 bug：帧率一变，结果就在"跑几次"之间横跳。
    ///
    /// 默认空实现，与 Tick 一致：绝大多数组件不需要固定步。
    virtual void FixedTick(float fixed_delta_time) { (void)fixed_delta_time; }

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