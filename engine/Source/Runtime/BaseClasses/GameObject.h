#pragma once

#include "ImmediatePtr.h"
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Serialize/TransferFunctions/SerializeTransfer.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"
#include "Runtime/Function/Framework/TagLayer/TagLayerManager.h"

#include <EASTL/vector.h>

#include <filesystem>
#include <vector>
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

    /// 每个固定步一次，分发给 m_Components（见 Component::FixedTick 的分工
    /// 说明）。与 Tick 走同一套激活 / enabled / 白名单判据。
    virtual void FixedTick(float fixed_delta_time);

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

    // ---- tag / layer -------------------------------------------------------
    //
    // Unity 的两个"给对象分类"的维度，用途不同、不可互换：
    //   tag  —— 一个字符串，**身份**（"Player"、"Enemy"）。查得到对象、比得出相等，
    //           不参与物理。
    //   layer—— 一个 0..31 的下标，**分组**（"UI"、"Water"）。不可读但极快，
    //           物理查询和摄像机 culling 靠它做位掩码过滤。
    //
    // 名字表本身在 TagLayerManager（Function/Framework/TagLayer），不在对象上：
    // 32 个层名和一份碰撞矩阵是**全项目一份**，挂在对象上会变成 N 份且会互相矛盾。

    /// TS 入口 `go.tag`。默认 `"Untagged"` —— 不存空串：空串会让
    /// `CompareTag("")` 在"没设标签"和"设了个空标签"之间分不出，于是
    /// `FindGameObjectsWithTag("")` 会把全场对象都捞出来。
    ZFUNCTION()
    const eastl::string& GetTag() const { return m_Tag; }

    /// 设置一个**已经存在**的标签。返回 false 表示这个标签没在 TagLayerManager
    /// 里登记过 —— 不静默接受未知标签，否则 Inspector 的拼写错误要到
    /// `CompareTag` 永远为 false 时才被发现，而那看起来就像"这个对象没被找到"。
    ///
    /// 传空串等于恢复 `"Untagged"`。
    ZFUNCTION(Name="SetTag")
    bool SetTagByName(const eastl::string& tag);

    /// Unity 的 `CompareTag`：比名字相等，且**比 GetTag() == x 更快更安全** ——
    /// 后者在标签不存在时静默 false，前者会（经 SetTag 的契约）保证标签存在。
    ZFUNCTION(Name="CompareTag")
    bool CompareTagName(const eastl::string& tag) const { return m_Tag == tag; }

    /// TS 入口 `go.layer`。0 = Default。越界写会被夹回 0..31：层号是物理查询
    /// 里的位下标，一个 47 会让后面的 `1 << layer` 变成 UB。
    ZFUNCTION()
    int32_t GetLayer() const { return m_Layer; }

    ZFUNCTION(Name="SetLayer")
    void SetLayerIndex(int32_t layer);

    ZSCRIPTPROPERTY(Name="tag", Getter="GetTag", Setter="SetTagByName")
    ZSCRIPTPROPERTY(Name="layer", Getter="GetLayer", Setter="SetLayerIndex")

    /// TS 入口 `GameObject.FindWithTag(tag)`：第一个带这个标签的**激活**对象。
    /// 找不到返回 nullptr。O(n)，同 `Find` —— 别放进 Update。
    ZFUNCTION(Name="FindWithTag")
    static GameObject* FindWithTagName(const eastl::string& tag);

    /// TS 入口 `GameObject.FindGameObjectsWithTag(tag)`：全部带这个标签的对象。
    /// 顺序不保证（底层是哈希表），需要稳定顺序自己排。
    ZFUNCTION(Name="FindGameObjectsWithTag")
    static std::vector<GameObject*> FindAllWithTagName(const eastl::string& tag);

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

    // ---- 层级查询（Unity：GetComponentInChildren / InParent）------------
    //
    // 三条规则的来源都是 Unity，且都踩过坑：
    //   * InChildren **包含自己** —— Unity 的 GetComponentInChildren 先查自己。
    //     不含自己的话 `GetComponentInChildren<Collider>()` 在"碰撞体就在本对象上"
    //     这个最常见的情况下返回 null，而调用方拿 null 当"没有碰撞体"。
    //   * 深度优先、**返回第一个**命中的（不是最近的一层）—— Unity 是先序遍历。
    //   * InParent **也包含自己**，然后逐级向上。

    /// TS 入口 `go.GetComponentInChildren(type)`：自己 + 所有后代里的第一个。
    ZFUNCTION(Name="GetComponentInChildren")
    Component* GetComponentInChildrenByTypeName(const std::string& component_type_name) const;

    /// TS 入口 `go.GetComponentsInChildren(type)`：自己 + 所有后代里的全部。
    /// 顺序是先序遍历（父在子前），与 Unity 一致。
    ZFUNCTION(Name="GetComponentsInChildren")
    std::vector<Component*> GetComponentsInChildrenByTypeName(const std::string& component_type_name) const;

    /// TS 入口 `go.GetComponentInParent(type)`：自己 + 所有祖先里的第一个。
    ZFUNCTION(Name="GetComponentInParent")
    Component* GetComponentInParentByTypeName(const std::string& component_type_name) const;

    // ---- 消息（Unity：SendMessage / BroadcastMessage / SendMessageUpwards）---
    //
    // 按名字调用脚本方法。只投递给 TypeScriptComponent（脚本是唯一能用字符串
    // 寻址方法的组件；C++ 组件的方法在编译期就定死了）。
    //
    // 返回**实际收到这条消息的组件数**。Unity 返回 void，这里返回 int 是有意的
    // 超集：0 表示"没人收到"，而"没人收到"绝大多数时候是拼错了方法名 —— 一个
    // 静默的 void 会让这种拼写错误活到上线。调用方可以忽略返回值。

    /// 本对象上的所有脚本组件。
    ZFUNCTION(Name="SendMessage")
    int SendMessageByName(const eastl::string& method_name);

    /// 带一个 `GameObject*` 参数的版本。Unity 的
    /// `SendMessage("OnCollisionEnter", other)` 走的就是这条路，而碰撞/触发
    /// 回调必须把"撞到谁"交给脚本 —— 没有它 `OnCollisionEnter(other)` 里的
    /// `other` 无从填起。
    ///
    /// `other` 为 null 时等价于无参版本（脚本收到 undefined）。
    ///
    /// ⚠️ 与 `SendMessageByName` 不同，这里**不**在"没人收到"时打警告：场景里
    /// 绝大多数碰撞体身上没有脚本，"没人接"是常态，打出来就是刷屏。
    int SendMessageWithOther(const char* method_name, GameObject* other);

    /// 本对象 + 所有后代（Unity 的 BroadcastMessage 是向下广播）。
    ZFUNCTION(Name="BroadcastMessage")
    int BroadcastMessageByName(const eastl::string& method_name);

    /// 本对象 + 所有祖先（Unity 的 SendMessageUpwards）。
    ZFUNCTION(Name="SendMessageUpwards")
    int SendMessageUpwardsByName(const eastl::string& method_name);

    /// TS 入口：`GameObject.Find(name)`。
    ///
    /// 按名字在当前关卡里找对象：先查关卡对象表，再查运行时生成的对象表。
    /// O(n)，与 Unity 的 GameObject.Find 同量级 —— 每帧 Update 里用它会拖垮帧率，
    /// 应该缓存结果。
    ZFUNCTION(Name="Find")
    static GameObject* FindByName(const eastl::string& name);

    /// TS 入口：`GameObject.Destroy(obj)`。
    ///
    /// **延迟**到帧末才真正销毁（Unity 同语义）：脚本可以在任何地方调用，包括
    /// 在某个对象的 OnUpdate 里销毁另一个对象 —— 立即销毁会让 Level::Tick 手里
    /// 正在用的迭代器失效。
    ///
    /// 只销毁传入的那一个。它的子对象**不会**跟着被销毁，而是提升到场景根 ——
    /// 否则父 Transform 没了，每个子对象的 m_Father 都指向已释放的内存。（Unity
    /// 是连整棵子树一起销毁；这里是刻意的选择，改动前先想清楚谁会依赖它。）
    ///
    /// 同一帧重复销毁同一个对象是安全的（内部去重）。
    ZFUNCTION(Name="Destroy")
    static void DestroyObject(GameObject* gobject);

    // ---- 激活状态 ----------------------------------------------------------
    //
    // 存储**复用** Transform::m_Enabled（即 Component::m_Enabled），不新建字段。
    // 三个既有消费方已经把它当 activeSelf 用了：GameObject::Tick、
    // BaseRenderer::SubmitRenderState、TimelineDirector 的 activation clip
    // （`runtime.saved_active = transform->isEnabled()`）。另起一个 m_ActiveSelf
    // 会立刻变成两个真值源 —— 一边开了另一边没开，谁也说不清。
    //
    // 与 Unity 一样区分两个概念：
    //   activeSelf        —— 自己这个开关，不管祖先；
    //   activeInHierarchy —— 自己**且**所有祖先都开着，决定 Tick / 渲染 / 光照。
    // 父级被停用时子对象的 activeSelf 不变，只有 activeInHierarchy 变 false，
    // 所以父级恢复后"本来就关着"的子对象不会跟着被打开。

    /// TS 入口：`go.SetActive(false)`，也可以 `go.activeSelf = false`。
    ///
    /// 值没变时直接返回（幂等）：不做无谓的子树遍历和渲染状态重发。
    ZFUNCTION(Name="SetActive")
    void SetActive(bool active);

    ZFUNCTION(Name="GetActiveSelf")
    bool GetActiveSelf() const;

    ZFUNCTION(Name="IsActiveInHierarchy")
    bool IsActiveInHierarchy() const;

    ZSCRIPTPROPERTY(Name="activeSelf", Getter="GetActiveSelf", Setter="SetActive")
    // 只读：层级状态是祖先决定的，不能直接写（Unity 同）。
    ZSCRIPTPROPERTY(Name="activeInHierarchy", Getter="IsActiveInHierarchy")

    /// 把新的激活状态推给本对象的组件，再沿 Transform 子树递归推给所有后代。
    /// 每个对象推的是**自己算出来**的 activeInHierarchy，不是上一层传下来的值 ——
    /// 后代里本来就关着的那些，父级恢复后仍然该是关的。
    ///
    /// 只由 SetActive 调用。
    void NotifyActiveChanged();

    /// 挂一个组件上来。
    ///
    /// Unity 的不变式（AddFirstTransformComponentInternal / EnsureUniqueTransform）：
    /// Transform 必须是 m_Components[0]，GameObject 创建时就挂上、且不允许第二个。
    /// 所以这里对 Transform 特殊处理 —— 插到最前而不是追加到末尾，否则后来补挂的
    /// Transform 会把索引 0 让给别人，GetTransform() 的 O(1) 快路径就废了。
    ///
    /// 实现在 .cpp：要靠 Cast<Transform> 判断，而 Transform.h 反向 include 了
    /// 本文件，头文件里拿不到完整类型（同 GetTransform 的理由）。
    void addComponent(Component* component);

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

    // Tag / layer. Serialized in Transfer() next to m_Name.
    //
    // 默认 "Untagged" / 0（Default），不是空串 / -1：老存档没有这两个字段，
    // SafeBinaryRead 会保持默认，一个 2019 年的关卡读进来必须仍然是个
    // 行为正常的对象，而不是"标签为空所以每条 CompareTag 都匹配"。
    eastl::string m_Tag {TagLayerManager::kUntagged};
    int32_t m_Layer {0};

    // we have to use the ReflectionPtr due to that the components need to be reflected
    // in editor, and it's polymorphism
    std::vector<ImmediatePtr<Component>> m_Components;

private:
    template<typename TransferFunction>
    void TransferComponents(TransferFunction& transfer);

    /// 把主 Transform 归位到 m_Components[0]（理由与实现见 .cpp）。
    /// 读进来的组件顺序由文件决定，不能假设它本来就在第一个。
    void EnsureTransformIsFirst();

    /// 先序遍历自己和所有后代，把命中 `component_type_name` 的组件收进 out。
    /// 自己排在前面（Unity 的 GetComponentsInChildren 顺序）。
    void CollectComponentsInChildren(const std::string& component_type_name,
                                     std::vector<Component*>& out) const;

    /// 把消息投递给本对象上的所有脚本组件，返回真正收到的个数。
    int DeliverMessageToSelfComponents(const eastl::string& method_name);

    /// `other` 为 null 表示无参调用。
    int DeliverMessageToSelfComponents(const eastl::string& method_name, GameObject* other);

    Container m_Component;
};
