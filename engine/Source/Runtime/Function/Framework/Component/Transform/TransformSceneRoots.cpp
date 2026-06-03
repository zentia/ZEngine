#include "Runtime/Function/Framework/Component/Transform/TransformSceneRoots.h"

#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Level/Level.h"

void TransformSceneRoots::Clear()
{
    Transform* root = m_FirstRoot;
    while (root != nullptr)
    {
        Transform* next = root->GetNextSceneRoot();
        root->SetSceneRootLinks(nullptr, nullptr);
        root = next;
    }
    m_FirstRoot = nullptr;
    m_LastRoot = nullptr;
}

void TransformSceneRoots::OnTransformBecameRoot(Transform* transform)
{
    if (transform == nullptr || transform->GetNextSceneRoot() != nullptr || transform == m_FirstRoot)
    {
        return;
    }

    transform->SetSceneRootLinks(m_LastRoot, nullptr);
    if (m_LastRoot != nullptr)
    {
        m_LastRoot->SetSceneRootLinks(m_LastRoot->GetPrevSceneRoot(), transform);
    }
    else
    {
        m_FirstRoot = transform;
    }
    m_LastRoot = transform;
}

void TransformSceneRoots::OnTransformLeftRoot(Transform* transform)
{
    if (transform == nullptr)
    {
        return;
    }

    Transform* const prev = transform->GetPrevSceneRoot();
    Transform* const next = transform->GetNextSceneRoot();
    if (prev != nullptr)
    {
        prev->SetSceneRootLinks(prev->GetPrevSceneRoot(), next);
    }
    else
    {
        m_FirstRoot = next;
    }

    if (next != nullptr)
    {
        next->SetSceneRootLinks(prev, next->GetNextSceneRoot());
    }
    else
    {
        m_LastRoot = prev;
    }

    transform->SetSceneRootLinks(nullptr, nullptr);
}

void TransformSceneRoots::ForEachRoot(const std::function<void(Transform*)>& visitor) const
{
    if (!visitor)
    {
        return;
    }

    for (Transform* root = m_FirstRoot; root != nullptr; root = root->GetNextSceneRoot())
    {
        visitor(root);
    }
}

TransformSceneRoots& GetTransformSceneRootsForLevel(Level* level)
{
    return level->GetTransformSceneRoots();
}
