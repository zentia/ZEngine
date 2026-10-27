#pragma once

#include "ImmediatePtr.h"
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Serialize/TransferFunctions/SerializeTransfer.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include "Runtime/BaseClasses/Cast.h"
#include "GameObject.generated.h"

// Forward-declared: Transform.h includes GameObject.h, so the real type is
// only available in the .cpp. A forward declaration is enough for the
// `Transform* GetTransform()` return type below.
class Transform;

ZCLASS(CustomTransfer)
class GameObject : public Object
{
    ZGENERATED_BODY(GameObject)

public:
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

    /// Merge the components authored in m_DefinitionUrl (a .prefab) into this
    /// object. A component type already present inline is treated as an
    /// override and is kept, so the scene's own Transform / volume settings win.
    /// Safe to call when the url is empty (no definition) -- a no-op.
    void ExpandDefinition();

    // The ZFUNCTION() markers below are no-ops to the compiler; ZHeaderTool
    // turns them into pesapi bindings plus `declare class GameObject` members
    // in the generated zengine.d.ts. Only types with a ZScript::Converter may
    // appear in a bound signature.
    ZFUNCTION()
    GObjectID GetID() const { return m_Id; }

    ZFUNCTION()
    void SetName(eastl::string name) { this->name = name; }

    ZFUNCTION()
    const eastl::string& GetName() const { return name; }

    const eastl::string& getDefinitionUrl() const { return m_DefinitionUrl; }

    // Setter for the prefab/object-definition url. Authoring tools (importers,
    // the .level.json -> .scene migrator) need to stamp the definition when
    // they synthesise a GameObject, since m_DefinitionUrl is otherwise only
    // reachable through deserialization.
    void setDefinitionUrl(eastl::string definition_url) { m_DefinitionUrl = std::move(definition_url); }

    ZFUNCTION()
    bool HasComponent(const std::string& component_type_name) const;

    /// First component whose type is `type_name` or derives from it (Unity's
    /// GetComponent-by-name semantics). Returns nullptr when the name does not
    /// resolve to a registered Type or no component matches.
    ///
    /// Deliberately NOT `tryGetComponent<T>()` -- that is a template and
    /// therefore invisible to ZHeaderTool, so script code could never reach it.
    ///
    /// ZFUNCTION: exposed as `GetComponent`, Unity's spelling. Scripts get the
    /// declared type `Component`, so cast-free access to a specific component
    /// means binding that type and giving it a getter (see Transform).
    ZFUNCTION(Name="GetComponent")
    Component* GetComponentByTypeName(const std::string& component_type_name) const;

    /// 按类型名**创建**一个组件并挂到本对象上（Unity `AddComponent<T>()` 的等价物）。
    ///
    /// 为什么必须新增这个方法而不是直接把 `addComponent(Component*)` 标 ZFUNCTION：
    /// 脚本既没有模板也没有 new，拿不到一个 Component 实例，所以一个"接收已有实例"
    /// 的入口对脚本毫无用处。要的是"给我一个类型名，引擎造好挂上"。
    ///
    /// 返回 nullptr 的情况：类型名没注册 / 是抽象类（无 factory）/ 不是 Component 派生。
    ///
    /// ZFUNCTION: 暴露成 `AddComponent`，Unity 的拼写。
    ZFUNCTION(Name="AddComponent")
    Component* AddComponentByTypeName(const std::string& component_type_name);

    /// Convenience wrapper used by the TS Behaviour binding to publish
    /// `this.transform`. Declared here and defined in the .cpp because
    /// Transform.h includes GameObject.h (the dependency points the other way).
    ZFUNCTION()
    Transform* GetTransform() const;

    /// TS 入口：`GameObject.Instantiate(path)`。
    ///
    /// 从 prefab 资产路径生成一份独立拷贝，登记进当前关卡的**运行时对象表**
    /// （不写进 .level 文件），返回它的根 GameObject。
    ///
    /// 实现放在 .cpp：PrefabUtility.h 在 Runtime/Resource 下，而本文件在
    /// BaseClasses，依赖方向不允许在这里 include（同 GetTransform 的理由）。
    ///
    /// 返回 nullptr 的情况：路径加载失败 / prefab 没有根对象 / 拿不到当前关卡 /
    /// clone 没有拿到新的 GObjectID。
    ZFUNCTION(Name="Instantiate")
    static GameObject* InstantiateFromPrefabPath(const std::filesystem::path& prefab_path);

    /// TS 入口：`GameObject.Find(name)`。
    ///
    /// 按名字在当前关卡里找对象：先查关卡对象表，再查运行时生成的对象表。
    /// O(n)，与 Unity 的 GameObject.Find 同量级 —— 每帧 Update 里用它会拖垮帧率，
    /// 应该缓存结果。
    ZFUNCTION(Name="Find")
    static GameObject* FindByName(const eastl::string& name);

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
    TComponent* tryGetComponent(const std::string& component_type_name)
    {
        (void)component_type_name;
        for (auto& component : m_Components)
        {
            TComponent* typed_component = Cast<TComponent>(static_cast<Component*>(component));
            if (typed_component != nullptr)
            {
                return typed_component;
            }
        }

        return nullptr;
    }

    template<typename TComponent>
    const TComponent* tryGetComponentConst(const std::string& component_type_name) const
    {
        (void)component_type_name;
        for (const auto& component : m_Components)
        {
            const TComponent* typed_component = Cast<TComponent>(static_cast<Component*>(component));
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
