// =============================================================================
// Component -- Unity 门面的实现
// =============================================================================
//
// 这个文件存在的唯一原因：Component.h 拿不到 GameObject / Transform 的完整
// 定义（两者都反向 include 了它），而 Unity 门面里几乎每个方法都要转发到宿主。
// 头文件里内联是不行的 —— 只能前向声明 + 在这里定义。
//
// 所有入口同一个形状：
//
//     if (m_ParentObject == nullptr) return <空值>;
//     return m_ParentObject->...;
//
// 这个空值分支不是防御性代码。组件确实有一段 `m_ParentObject == nullptr`
// 的真实窗口 —— MemoryManager 造出来之后、PostLoadResource 挂上宿主之前
// （Component.h 里 m_ParentObject 的初始化注释记的就是它）。脚本没理由知道
// 这个窗口，但它撞上时也必须得到一个合法的空值而不是崩。
// =============================================================================

#include "Runtime/Function/Framework/Component/Component.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/TagLayer/TagLayerManager.h"

Transform* Component::GetTransform() const
{
    return (m_ParentObject != nullptr) ? m_ParentObject->GetTransform() : nullptr;
}

const eastl::string& Component::GetName() const
{
    // Unity：组件与它挂的那个 GameObject 共享名字。没有宿主时退回 Object::name
    // —— 那时它是唯一可信的来源（比如一个还躺在 prefab 资产里的模板组件）。
    return (m_ParentObject != nullptr) ? m_ParentObject->GetName() : name;
}

void Component::SetName(const eastl::string& new_name)
{
    if (m_ParentObject != nullptr)
    {
        m_ParentObject->SetName(new_name);
        return;
    }
    name = new_name;
}

eastl::string Component::GetTag() const
{
    return (m_ParentObject != nullptr) ? eastl::string(m_ParentObject->GetTag())
                                       : eastl::string(TagLayerManager::kUntagged);
}

bool Component::SetTagByName(const eastl::string& tag)
{
    // 没有宿主时返回 false 而不是 true：true 会让脚本以为标签设上了，而下一个
    // CompareTag 又是 false。静默的失败承诺比失败本身贵得多。
    return (m_ParentObject != nullptr) && m_ParentObject->SetTagByName(tag);
}

bool Component::CompareTagName(const eastl::string& tag) const
{
    // 无宿主时比的是**GetTag 会返回的那个值**，不是一律 false：否则
    // `comp.tag === "Untagged"` 与 `comp.CompareTag("Untagged")` 会给出两个
    // 相反的答案，而这种自相矛盾的 API 只能靠读源码才能用对。
    return (m_ParentObject != nullptr) ? m_ParentObject->CompareTagName(tag)
                                       : (eastl::string(TagLayerManager::kUntagged) == tag);
}

Component* Component::GetComponentByTypeName(const std::string& component_type_name) const
{
    return (m_ParentObject != nullptr) ? m_ParentObject->GetComponentByTypeName(component_type_name)
                                       : nullptr;
}

std::vector<Component*> Component::GetComponentsByTypeName(const std::string& component_type_name) const
{
    return (m_ParentObject != nullptr) ? m_ParentObject->GetComponentsByTypeName(component_type_name)
                                       : std::vector<Component*> {};
}

Component* Component::GetComponentInChildrenByTypeName(const std::string& component_type_name) const
{
    return (m_ParentObject != nullptr) ? m_ParentObject->GetComponentInChildrenByTypeName(component_type_name)
                                       : nullptr;
}

std::vector<Component*> Component::GetComponentsInChildrenByTypeName(
    const std::string& component_type_name) const
{
    return (m_ParentObject != nullptr) ? m_ParentObject->GetComponentsInChildrenByTypeName(component_type_name)
                                       : std::vector<Component*> {};
}

Component* Component::GetComponentInParentByTypeName(const std::string& component_type_name) const
{
    return (m_ParentObject != nullptr) ? m_ParentObject->GetComponentInParentByTypeName(component_type_name)
                                       : nullptr;
}

std::vector<Component*> Component::GetComponentsInParentByTypeName(
    const std::string& component_type_name) const
{
    return (m_ParentObject != nullptr) ? m_ParentObject->GetComponentsInParentByTypeName(component_type_name)
                                       : std::vector<Component*> {};
}

int Component::SendMessageByName(const eastl::string& method_name)
{
    return (m_ParentObject != nullptr) ? m_ParentObject->SendMessageByName(method_name) : 0;
}

int Component::BroadcastMessageByName(const eastl::string& method_name)
{
    return (m_ParentObject != nullptr) ? m_ParentObject->BroadcastMessageByName(method_name) : 0;
}

int Component::SendMessageUpwardsByName(const eastl::string& method_name)
{
    return (m_ParentObject != nullptr) ? m_ParentObject->SendMessageUpwardsByName(method_name) : 0;
}
