#pragma once

#include <functional>

class Level;
class Transform;

/// Per-level doubly-linked list of scene-root transforms (Unity SceneRootNode equivalent).
class TransformSceneRoots
{
public:
    void Clear();

    void OnTransformBecameRoot(Transform* transform);
    void OnTransformLeftRoot(Transform* transform);

    void ForEachRoot(const std::function<void(Transform*)>& visitor) const;

    Transform* GetFirstRoot() const { return m_FirstRoot; }

private:
    Transform* m_FirstRoot {nullptr};
    Transform* m_LastRoot {nullptr};
};

TransformSceneRoots& GetTransformSceneRootsForLevel(Level* level);
