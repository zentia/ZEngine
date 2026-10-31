#include "GameObject.h"

#include "ObjectDefines.h"
#include "Runtime/Application/Application.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/Rigidbody/RigidbodyComponent.h"
#include "Runtime/Function/Framework/Component/Script/TypeScriptComponent.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Prefab/PrefabUtility.h"

#include <cassert>
#include <filesystem>
#include <typeinfo>
#include <unordered_set>
#include <utility>

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
    // 层级激活判定（Unity activeInHierarchy 语义）：自己开着还不够，祖先里只要有
    // 一个关着就不 Tick。
    //
    // 这里以前只查自己的 Transform::m_Enabled，于是"父级停用、子对象照跑" —— 而
    // 渲染侧后来认了层级（BaseRenderer::SubmitRenderState 查 IsActiveInHierarchy），
    // 两边一不一致就变成"看不见但还在动"。统一到同一个判定上。
    //
    // Transform::m_Enabled is the GameObject active flag (Unity activeSelf analogue);
    // IsActiveInHierarchy() walks the Transform parents on top of it.
    if (!IsActiveInHierarchy())
    {
        return;
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

void GameObject::FixedTick(float fixed_delta_time)
{
    // 与 Tick 完全同构：同样的层级激活判定、同样的 enabled 判定、同样的
    // g_editorTickComponentTypes 白名单。两边判定必须一致 —— 出现"FixedUpdate
    // 跑了但 Update 没跑"（或反之）时，组件状态会静默地在两套节奏之间错位。
    if (!IsActiveInHierarchy())
    {
        return;
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

        component->FixedTick(fixed_delta_time);
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

// Unity 的约定：`Scripting::ScriptingWrapperForTransformComponentOf` 直接取
// `go.GetComponentAtIndex(0)` 再 `DebugAssert(firstComponent.Is<Transform>())`，
// `GameObject::ReplaceTransformComponentInternal` 取 `QueryComponentAtIndex<Transform>(0)`
// —— Transform 永远是第一个组件，不需要按类型查。
//
// 原来这里是 `GetComponentByTypeName("Transform")`：TypeManager 查表 + 逐组件
// IsBaseOf 扫描，而 GetTransform 是热路径（渲染提交 / AI / 物理每帧都调它），
// 那份开销是纯浪费。改成按索引取，O(1)。
//
// 不变式由三处共同保证：
//   * addComponent()                —— Transform 插到最前；
//   * RebuildRuntimeComponents()    —— 反序列化顺序由文件决定，归位一次；
//   * load()                        —— prefab 展开是追加在末尾的，也要归位。
Transform* GameObject::GetTransform() const
{
    if (!m_Components.empty())
    {
        Component* first = static_cast<Component*>(m_Components.front());
        if (Transform* transform = Cast<Transform>(first))
        {
            return transform;
        }
    }

    // 兜底：老文件 / 直接改 EditorGetComponentsRef() 的路径仍可能破坏顺序，这时
    // 退化成线性扫描 —— 宁可慢，也不能返回 nullptr 让调用方以为"没有 Transform"。
    for (const ImmediatePtr<Component>& component : m_Components)
    {
        if (Transform* transform = Cast<Transform>(static_cast<Component*>(component)))
        {
            return transform;
        }
    }

    return nullptr;
}

void GameObject::addComponent(Component* component)
{
    if (component == nullptr)
    {
        return;
    }

    // Transform 永远是 m_Components[0]（见 GetTransform 的说明）。后挂上来的
    // Transform —— 脚本 AddComponent、迁移工具给无空间放置的定义补的根
    // Transform —— 也必须插到最前，不能 push_back。
    if (Cast<Transform>(component) != nullptr)
    {
        m_Components.insert(m_Components.begin(), component);
        return;
    }

    m_Components.emplace_back(component);
}

void GameObject::EnsureTransformIsFirst()
{
    // 只交换，不重排：把主 Transform 与 index 0 对调，其余组件的相对顺序保持
    // 原样，免得影响任何按索引读组件的旧代码。
    if (m_Components.size() < 2)
    {
        return;
    }

    Component* first = static_cast<Component*>(m_Components.front());
    if (Cast<Transform>(first) != nullptr)
    {
        return;
    }

    for (auto it = m_Components.begin() + 1; it != m_Components.end(); ++it)
    {
        if (Cast<Transform>(static_cast<Component*>(*it)) == nullptr)
        {
            continue;
        }

        std::swap(m_Components.front(), *it);
        return;
    }
}

void GameObject::SetActive(bool active)
{
    Transform* transform = tryGetComponent(Transform);
    if (transform == nullptr)
    {
        // 没有 Transform 就没有"激活"这件事（也无从挂在层级里）。不静默吞掉：
        // 脚本拿到的是"设了但没生效"，比崩了更难查。
        LOG_ERROR(ZObject, "GameObject::SetActive: '{}' has no Transform -- cannot change active state", name.c_str());
        return;
    }

    if (transform->isEnabled() == active)
    {
        return;
    }

    transform->SetEnabled(active);

    // 必须主动推：对象停用后 GameObject::Tick 直接 return，组件的 Tick 再也跑不到，
    // 它们没有任何机会自己去撤渲染实体 / 关光源。
    NotifyActiveChanged();
}

bool GameObject::GetActiveSelf() const
{
    const Transform* transform = tryGetComponentConst(Transform);
    // 没有 Transform 时按"激活"处理 —— 与 GameObject::Tick / BaseRenderer 的既有
    // 约定一致（两者都把 null Transform 当成不影响激活判断）。
    return transform == nullptr || transform->isEnabled();
}

bool GameObject::IsActiveInHierarchy() const
{
    const Transform* transform = tryGetComponentConst(Transform);
    if (transform == nullptr)
    {
        return true;
    }
    if (!transform->isEnabled())
    {
        return false;
    }
    // 与 Transform::GetRoot() 同样的写法（同样不加环保护）：父子链成环的话
    // GetRoot() 早就先死循环了，这里再单独加一道上限只会掩住真正的问题。
    for (const Transform* ancestor = transform->GetParent(); ancestor != nullptr; ancestor = ancestor->GetParent())
    {
        if (!ancestor->isEnabled())
        {
            return false;
        }
    }
    return true;
}

void GameObject::NotifyActiveChanged()
{
    const bool active_in_hierarchy = IsActiveInHierarchy();
    for (auto& component : m_Components)
    {
        Component* component_ptr = static_cast<Component*>(component);
        if (component_ptr != nullptr)
        {
            component_ptr->OnOwnerActiveChanged(active_in_hierarchy);
        }
    }

    // 子树：父级开关一变，整棵子树的 activeInHierarchy 都变了，逐个重推。
    // 递归而不是显式栈：层级深度是场景结构的深度，不是对象数量。
    Transform* transform = tryGetComponent(Transform);
    if (transform == nullptr)
    {
        return;
    }
    for (const auto& child : transform->GetChildren())
    {
        Transform* child_transform = static_cast<Transform*>(child);
        if (child_transform == nullptr)
        {
            continue;
        }
        GameObject* child_object = child_transform->GetParentObject();
        if (child_object == nullptr)
        {
            continue;
        }
        child_object->NotifyActiveChanged();
    }
}

namespace
{
    Level* GetActiveLevel(const char* caller)
    {
        WorldManager* world_manager = GET_SYSTEM(WorldManager);
        if (world_manager == nullptr)
        {
            LOG_ERROR(ZObject, "{}: WorldManager unavailable", caller);
            return nullptr;
        }
        Level* level = world_manager->getCurrentActiveLevel();
        if (level == nullptr)
        {
            LOG_ERROR(ZObject, "{}: no active level", caller);
            return nullptr;
        }
        return level;
    }
}  // namespace

GameObject* GameObject::InstantiateFromPrefabPath(const std::filesystem::path& prefab_path)
{
    if (prefab_path.empty())
    {
        LOG_WARNING(ZObject, "GameObject.Instantiate: empty prefab path");
        return nullptr;
    }

    Level* level = GetActiveLevel("GameObject.Instantiate");
    if (level == nullptr)
    {
        return nullptr;
    }

    GameObject* root = PrefabUtility::InstantiateFromPath(prefab_path);
    if (root == nullptr)
    {
        LOG_ERROR(ZObject,
                  "GameObject.Instantiate: failed to instantiate '{}'",
                  prefab_path.generic_string());
        return nullptr;
    }

    // CloneSubtree 走的是**序列化往返**（TransferUtility::CloneObjectViaSerialization），
    // m_Id 会跟着模板一起被拷出来 —— 两次 Instantiate 同一个 prefab 就得到两个
    // ID 相同的 GameObject，第二个进运行时表时会被 RegisterRuntimeGObject 拒掉
    // （它故意不覆盖已有条目，否则会丢掉前一个对象的唯一引用）。
    // 所以整棵子树都重新发一个 GObjectID。
    //
    // 用显式栈而不是递归：clone 出来的子树深度由 prefab 决定，深 prefab 递归有
    // 爆栈风险，而且这样也不用为递归 lambda 引入 <functional>。
    std::vector<GameObject*> pending {root};
    while (!pending.empty())
    {
        GameObject* go = pending.back();
        pending.pop_back();
        if (go == nullptr)
        {
            continue;
        }

        const GObjectID fresh_id = ObjectIDAllocator::Alloc();
        if (fresh_id == k_invalid_gobject_id)
        {
            LOG_ERROR(ZObject, "GameObject.Instantiate: GObjectID allocator exhausted");
            return nullptr;
        }
        // m_Id 是 protected —— 静态成员函数的函数体里可以通过对象访问。
        go->m_Id = fresh_id;

        if (Transform* transform = go->GetTransform())
        {
            for (const PPtr<Transform>& child : transform->GetChildren())
            {
                Transform* child_transform = child;
                if (child_transform == nullptr)
                {
                    continue;
                }
                if (GameObject* child_object = child_transform->GetParentObject())
                {
                    pending.push_back(child_object);
                }
            }
        }
    }

    if (!level->RegisterRuntimeGObject(root))
    {
        LOG_ERROR(ZObject,
                  "GameObject.Instantiate: could not register '{}' ('{}')",
                  root->GetName().c_str(),
                  prefab_path.generic_string());
        return nullptr;
    }

    return root;
}

GameObject* GameObject::FindByName(const eastl::string& name)
{
    if (name.empty())
    {
        return nullptr;
    }

    Level* level = GetActiveLevel("GameObject.Find");
    if (level == nullptr)
    {
        return nullptr;
    }
    return level->FindGObjectByName(name);
}

bool GameObject::SetTagByName(const eastl::string& tag)
{
    // 空串 = 恢复默认。Unity 的 Inspector 不允许空标签，这里同理：空标签在
    // `CompareTag` 上是"永远不匹配"，看着像 bug。
    if (tag.empty())
    {
        m_Tag = TagLayerManager::kUntagged;
        return true;
    }

    // 未知标签要拒绝并说清楚。静默接受的话，脚本里的一个拼写错误会一直等到
    // `CompareTag` 返回 false 才暴露，而那个现象与"对象没被找到"一模一样。
    TagLayerManager* tags = GET_SYSTEM(TagLayerManager);
    if (tags != nullptr && !tags->HasTag(tag))
    {
        LOG_WARNING(ZObject,
                 "GameObject '{}': tag '{}' is not registered in TagLayerManager; "
                 "keeping '{}'. Add it first or fix the spelling -- an unregistered "
                 "tag makes every CompareTag silently false.",
                 name.c_str(), tag.c_str(), m_Tag.c_str());
        return false;
    }

    m_Tag = tag;
    return true;
}

void GameObject::SetLayerIndex(int32_t layer)
{
    // 夹回合法范围而不是拒绝：层号是物理查询里的位下标，`1 << 47` 是 UB，
    // 而一个"没生效"的 SetLayer 比一个"变成 Default"的 SetLayer 更难查。
    if (layer < 0 || layer >= TagLayerManager::kLayerCount)
    {
        LOG_WARNING(ZObject,
                 "GameObject '{}': layer {} out of range 0..{}; clamped to 0 (Default).",
                 name.c_str(), layer, TagLayerManager::kLayerCount - 1);
        m_Layer = 0;
        return;
    }
    m_Layer = layer;

    // 层号是 Jolt 的 object layer（见 Jolt/Utils.h），所以改层必须同步到刚体，
    // 否则 IgnoreLayerCollision 对这个对象不生效 —— 而它能编译、能运行、没有报错。
    if (RigidBodyComponent* rigid_body = tryGetComponent(RigidBodyComponent))
    {
        rigid_body->RefreshBodyLayer();
    }
}

namespace
{
    /// 标签查询的遍历体。两张表都要看：关卡对象表 + Instantiate 出来的运行时对象
    /// 表（Level::FindGObjectByName 同一套顺序）。
    ///
    /// `active_only` 对齐 Unity：`FindWithTag` / `FindGameObjectsWithTag` 只返回
    /// **激活**对象。不过滤的话，一个"已经关掉的敌人"会被逻辑继续当成活的 ——
    /// 而它看起来就待在场景里，因为停用的对象仍然在关卡表里。
    template<typename Fn>
    void ForEachGameObjectInActiveLevel(Fn&& fn)
    {
        Level* level = GetActiveLevel("GameObject.FindByTag");
        if (level == nullptr)
        {
            return;
        }

        for (const auto& id_object_pair : level->getAllGObjects())
        {
            if (id_object_pair.second != nullptr)
            {
                fn(id_object_pair.second.get());
            }
        }
        for (const auto& id_object_pair : level->GetRuntimeGObjects())
        {
            if (id_object_pair.second != nullptr)
            {
                fn(id_object_pair.second);
            }
        }
    }
}  // namespace

void GameObject::CollectComponentsInChildren(const std::string& component_type_name,
                                            std::vector<Component*>& out) const
{
    // 自己先：Unity 的 GetComponentInChildren 含自己，且先序遍历父在子前。
    if (Component* own = GetComponentByTypeName(component_type_name))
    {
        out.push_back(own);
    }

    const Transform* self_transform = GetTransform();
    if (self_transform == nullptr)
    {
        return;
    }

    for (const PPtr<Transform>& child : self_transform->GetChildren())
    {
        Transform* child_transform = child;
        if (child_transform == nullptr)
        {
            continue;
        }
        GameObject* child_object = child_transform->GetParentObject();
        if (child_object == nullptr)
        {
            continue;
        }
        child_object->CollectComponentsInChildren(component_type_name, out);
    }
}

Component* GameObject::GetComponentInChildrenByTypeName(const std::string& component_type_name) const
{
    std::vector<Component*> found;
    CollectComponentsInChildren(component_type_name, found);
    return found.empty() ? nullptr : found.front();
}

std::vector<Component*> GameObject::GetComponentsInChildrenByTypeName(
    const std::string& component_type_name) const
{
    std::vector<Component*> found;
    CollectComponentsInChildren(component_type_name, found);
    return found;
}

Component* GameObject::GetComponentInParentByTypeName(const std::string& component_type_name) const
{
    // 含自己 —— 与 InChildren 同一个理由。
    if (Component* own = GetComponentByTypeName(component_type_name))
    {
        return own;
    }

    const Transform* self_transform = GetTransform();
    if (self_transform == nullptr)
    {
        return nullptr;
    }

    for (Transform* parent = self_transform->GetParent(); parent != nullptr;
         parent = parent->GetParent())
    {
        GameObject* parent_object = parent->GetParentObject();
        if (parent_object == nullptr)
        {
            continue;
        }
        if (Component* hit = parent_object->GetComponentByTypeName(component_type_name))
        {
            return hit;
        }
    }
    return nullptr;
}

std::vector<Component*> GameObject::GetComponentsByTypeName(const std::string& component_type_name) const
{
    const Type* wanted = TypeManager::GetInstance().ClassNameToType(component_type_name.c_str());
    if (wanted == nullptr)
        return {};

    std::vector<Component*> found;
    for (const auto& component : m_Components)
    {
        Component* candidate = static_cast<Component*>(component);
        if (candidate == nullptr)
            continue;
        const Type* runtime_type = candidate->GetMostDerivedType();
        // IsBaseOf 而不是名字相等：`GetComponents("Component")` 必须能捞到所有
        // 组件，否则脚本只能按具体类型一个个查（与 GetComponentByTypeName 同源）。
        if (runtime_type != nullptr && wanted->IsBaseOf(runtime_type->GetRuntimeTypeIndex()))
            found.push_back(candidate);
    }
    return found;
}

std::vector<Component*> GameObject::GetComponentsInParentByTypeName(
    const std::string& component_type_name) const
{
    // 与 GetComponentInParent 同一条先序遍历（含自己、由近及远），只是中途
    // 不提前返回 —— 两个方法必须走**同一条路径**，否则"第一个"可能不是"全部
    // 里的第一个"，那种不一致比查不到更难查。
    std::vector<Component*> found;
    for (Component* own : GetComponentsByTypeName(component_type_name))
    {
        found.push_back(own);
    }

    const Transform* self_transform = GetTransform();
    if (self_transform == nullptr)
    {
        return found;
    }

    for (Transform* parent = self_transform->GetParent(); parent != nullptr;
         parent = parent->GetParent())
    {
        GameObject* parent_object = parent->GetParentObject();
        if (parent_object == nullptr)
        {
            continue;
        }
        for (Component* hit : parent_object->GetComponentsByTypeName(component_type_name))
        {
            found.push_back(hit);
        }
    }
    return found;
}

int GameObject::DeliverMessageToSelfComponents(const eastl::string& method_name)
{
    return DeliverMessageToSelfComponents(method_name, nullptr);
}

int GameObject::DeliverMessageToSelfComponents(const eastl::string& method_name, GameObject* other)
{
    int delivered = 0;
    for (auto& component : m_Components)
    {
        Component* base = component;
        if (base == nullptr)
        {
            continue;
        }
        // 只有脚本组件能按名字寻址方法。C++ 组件的方法在编译期定死，
        // 字符串查找在这里没有任何落点。
        TypeScriptComponent* script = Cast<TypeScriptComponent>(base);
        if (script == nullptr)
        {
            continue;
        }
        const bool ok = (other != nullptr) ? script->InvokeMethodWithOther(method_name.c_str(), other)
                                           : script->InvokeMethod(method_name.c_str());
        if (ok)
        {
            ++delivered;
        }
    }
    return delivered;
}

int GameObject::SendMessageWithOther(const char* method_name, GameObject* other)
{
    if (method_name == nullptr || method_name[0] == '\0')
    {
        return 0;
    }
    return DeliverMessageToSelfComponents(eastl::string(method_name), other);
}

int GameObject::SendMessageByName(const eastl::string& method_name)
{
    if (method_name.empty())
    {
        return 0;
    }
    const int delivered = DeliverMessageToSelfComponents(method_name);
    if (delivered == 0)
    {
        // 说出来而不是静默。"没人收到"绝大多数时候是方法名拼错了，而一个
        // 静默的 SendMessage 会让这个拼写错误一直活到上线。
        LOG_WARNING(ZObject,
                    "SendMessage('{}') on '{}': no live script component received it "
                    "(method name misspelled, or the component has not bound yet?)",
                    method_name.c_str(), name.c_str());
    }
    return delivered;
}

int GameObject::BroadcastMessageByName(const eastl::string& method_name)
{
    if (method_name.empty())
    {
        return 0;
    }

    int delivered = DeliverMessageToSelfComponents(method_name);

    const Transform* self_transform = GetTransform();
    if (self_transform != nullptr)
    {
        for (const PPtr<Transform>& child : self_transform->GetChildren())
        {
            Transform* child_transform = child;
            if (child_transform == nullptr)
            {
                continue;
            }
            GameObject* child_object = child_transform->GetParentObject();
            if (child_object != nullptr)
            {
                delivered += child_object->BroadcastMessageByName(method_name);
            }
        }
    }
    return delivered;
}

int GameObject::SendMessageUpwardsByName(const eastl::string& method_name)
{
    if (method_name.empty())
    {
        return 0;
    }

    int delivered = DeliverMessageToSelfComponents(method_name);

    const Transform* self_transform = GetTransform();
    if (self_transform != nullptr)
    {
        // 只往上走一层然后交给父对象自己的 SendMessageUpwards：把"自己"这一段
        // 的判定留在同一个函数里，避免两处规则漂移（一处含自己、一处不含）。
        Transform* parent = self_transform->GetParent();
        if (parent != nullptr)
        {
            GameObject* parent_object = parent->GetParentObject();
            if (parent_object != nullptr)
            {
                delivered += parent_object->SendMessageUpwardsByName(method_name);
            }
        }
    }
    return delivered;
}

GameObject* GameObject::FindWithTagName(const eastl::string& tag)
{
    if (tag.empty())
    {
        return nullptr;
    }

    GameObject* found = nullptr;
    ForEachGameObjectInActiveLevel([&](GameObject* gobject) {
        if (found != nullptr)
        {
            return;
        }
        if (gobject->IsActiveInHierarchy() && gobject->CompareTagName(tag))
        {
            found = gobject;
        }
    });
    return found;
}

std::vector<GameObject*> GameObject::FindAllWithTagName(const eastl::string& tag)
{
    // std::vector 而不是 eastl::vector：ZScript::Converter 只特化了 std::vector
    // （ScriptBinding.h），eastl 那个会撞主模板的 static_assert，编译期就红。
    std::vector<GameObject*> out;
    if (tag.empty())
    {
        // 空查询返回空，不是"全部"。返回全部会把 `if (results.length === 0)`
        // 这条最常见的判空路径变成永远走不到 —— 而它看起来只是"这次真的没找到"。
        return out;
    }

    ForEachGameObjectInActiveLevel([&](GameObject* gobject) {
        if (gobject->IsActiveInHierarchy() && gobject->CompareTagName(tag))
        {
            out.push_back(gobject);
        }
    });
    return out;
}

void GameObject::DestroyObject(GameObject* gobject)
{
    if (gobject == nullptr)
    {
        return;
    }

    Level* level = GetActiveLevel("GameObject.Destroy");
    if (level == nullptr)
    {
        return;
    }
    level->QueueDestroy(gobject->GetID());
}

Component* GameObject::AddComponentByTypeName(const std::string& component_type_name)
{
    const Type* wanted = TypeManager::GetInstance().ClassNameToType(component_type_name.c_str());
    if (wanted == nullptr)
    {
        LOG_WARNING(ZObject, "AddComponent: '{}' is not a registered reflected type", component_type_name.c_str());
        return nullptr;
    }
    if (wanted->GetFactory() == nullptr)
    {
        LOG_WARNING(ZObject, "AddComponent: '{}' has no factory (abstract type?)", component_type_name.c_str());
        return nullptr;
    }

    // Transform 是每个 GameObject 有且仅有一个的（GetTransform() 取 m_Components[0]，
    // 编辑器也因此把它从可添加列表里藏起来）。脚本绕过编辑器直接加第二个会让
    // 层级关系变得不确定，这里挡掉并返回已有的那个 —— 与 Unity 抛异常不同，
    // 脚本侧更希望拿到能用的东西而不是崩。
    //
    // 放在 Produce() 之前，免得白造一个对象再销毁。
    const Type* transform_type = TypeManager::GetInstance().ClassNameToType("Transform");
    if (transform_type != nullptr && transform_type->IsBaseOf(wanted->GetRuntimeTypeIndex()))
    {
        Transform* existing = GetTransform();
        if (existing != nullptr)
        {
            LOG_WARNING(ZObject, "AddComponent: '{}' already has a Transform, returning the existing one", GetName().c_str());
            return existing;
        }
    }

    ObjectManager* object_manager = GET_SYSTEM(ObjectManager);
    if (object_manager == nullptr)
    {
        LOG_ERROR(ZObject, "AddComponent: '{}' failed, ObjectManager unavailable", component_type_name.c_str());
        return nullptr;
    }

    // Produce() -> `new T()` via the generated ProduceHelper，然后分配一个真的
    //（负数）堆 InstanceID。MemoryManager::CreateObject 会把 m_InstanceID 留在 0，
    // 那样组件对 PPtr / ImmediatePtr 和场景序列化器都是不可见的。
    Object* raw = object_manager->Produce(wanted, 0);
    if (raw == nullptr)
    {
        LOG_WARNING(ZObject, "AddComponent: '{}' could not be instantiated", component_type_name.c_str());
        return nullptr;
    }
    object_manager->AllocateAndAssignInstanceID(raw);

    Component* component = Cast<Component>(raw);
    if (component == nullptr)
    {
        LOG_WARNING(ZObject, "AddComponent: '{}' is not a Component", component_type_name.c_str());
        object_manager->Destroy(raw);
        return nullptr;
    }

    // PostLoadResource 必须早于 addComponent：它绑定 m_ParentObject，而子类的
    // 资源路径解析 / 子系统注册都依赖它。AnimationComponent、TypeScriptComponent、
    // AudioSourceComponent 在没跑过它之前被访问会崩或静默失效。
    component->PostLoadResource(this);
    addComponent(component);

    LOG_INFO(ZObject, "GameObject '{}': added component '{}'", GetName().c_str(), component_type_name.c_str());
    return component;
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
    m_Tag = object_instance_res.m_Tag;
    m_Layer = object_instance_res.m_Layer;
    if (m_Tag.empty())
    {
        // 老 prefab / 老场景没有这个字段。这里补默认而不是留空：空标签会让
        // CompareTag("") 在"没设"和"设为空"之间分不出。
        m_Tag = TagLayerManager::kUntagged;
    }
    ExpandDefinition();

    // ExpandDefinition 是往末尾追加的，源文件里的顺序也未必是 Transform 打头
    // （老场景 / 老 prefab）。PostLoadResource 之前归位，组件初始化时看到的
    // 就已经是最终顺序。
    EnsureTransformIsFirst();

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
    out_object_instance_res.m_Tag = m_Tag;
    out_object_instance_res.m_Layer = m_Layer;
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
    // tag / layer。与 m_Name 同一条规则：早于这两个字段的存档读不回来，
    // SafeBinaryRead 会保持默认值（"Untagged" / 0），所以老关卡读进来仍然是
    // 行为正常的对象，而不是"标签为空于是每条 CompareTag 都匹配"。
    transfer.Transfer(m_Tag, "m_Tag");
    transfer.Transfer(m_Layer, "m_Layer");
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

    // 文件里组件的顺序不受我们控制（Unity 的 EnsureUniqueTransform 也是在这个
    // 阶段收口）。归位一次，让 GetTransform() 的 index-0 快路径对读进来的对象
    // 也成立。
    EnsureTransformIsFirst();
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