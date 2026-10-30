#pragma once

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

class CameraComponent;
class Character;
class Level;
class Object;
class ObjectInstanceRes;
class PhysicsScene;
#include "Runtime/Function/Framework/Component/Transform/TransformSceneRoots.h"

using LevelObjectsMap = std::unordered_map<GObjectID, std::shared_ptr<GameObject>>;

/// Extension points on the level object graph. Runtime knows nothing about
/// editor-only types (PrefabInstance lives in the Editor module), so the editor
/// installs these to keep its bookkeeping objects in the same scene file as the
/// GameObjects they refer to. Every hook is optional; a null hook is skipped.
struct LevelSerializationHooks
{
    /// Level::save, after every live GameObject and Component has been collected:
    /// append extra objects to write into the same file. They may refer to the
    /// level's GameObjects / Components by PPtr (live GameObjects carry instance
    /// IDs, see Level::CreateObject).
    void (*collect_extra_objects)(const Level& level, std::vector<Object*>& out) = nullptr;

    /// Level::LoadObjectGraph, after the live objects exist. `entries` is the
    /// whole graph as read from the file. Level builds each live GameObject as a
    /// copy of the one read from the file (components are shared, the GameObject
    /// itself is not), so `file_to_live` maps the instance ID of every GameObject
    /// read from the file to the live GameObject that replaced it -- a PPtr to the
    /// former must be retargeted to the latter.
    void (*on_loaded)(Level& level,
                      const std::vector<std::pair<int64_t, Object*>>& entries,
                      const std::unordered_map<int32_t, GameObject*>& file_to_live) = nullptr;

    /// Level::Unload, before the object table is cleared.
    void (*on_unloading)(Level& level) = nullptr;
};

/// The main class to manage all game objects
class Level
{
public:
    virtual ~Level() {};

    bool load(const eastl::string& level_res_url);

    /// The part of load() after the file has been read: build the live level from
    /// an object graph already in memory (LevelRes header + GameObjects +
    /// Components). load() calls it with create_physics_scene = true; tools and
    /// headless tests that have no physics backend pass false.
    bool LoadObjectGraph(const std::vector<std::pair<int64_t, Object*>>& entries, bool create_physics_scene = true);

    void Unload();

    static void SetSerializationHooks(const LevelSerializationHooks& hooks);
    static const LevelSerializationHooks& GetSerializationHooks();

    bool save();

    bool IsDirty() const { return m_IsDirty; }
    void MarkDirty() { m_IsDirty = true; }
    void ClearDirty() { m_IsDirty = false; }

    /// 每帧一次：跑组件 Update（帧间隔），再清销毁队列、派发变换变更。
    void Tick(float delta_time);

    /// 每个固定步一次：跑组件 FixedUpdate，**并推进物理**。
    ///
    /// 物理放在这里而不是 Tick 里是这次改动的核心：物理是"每步积分"的东西，
    /// 吃帧间隔就意味着模拟结果跟帧率绑死（换机器复现不了、联网对不上）。
    /// 固定步之后物理以恒定步长推进，与渲染帧率无关。
    void FixedTick(float fixed_delta_time);

    const eastl::string& getLevelResUrl() const { return m_LevelResUrl; }
    void setLevelResUrl(const eastl::string& level_res_url) { m_LevelResUrl = level_res_url; }

    const LevelObjectsMap& getAllGObjects() const { return m_Gobjects; }

    std::weak_ptr<GameObject> GetGObjectByID(GObjectID go_id) const;

    // --- 运行时生成的对象（Instantiate 产物）-------------------------------
    //
    // 为什么单开一张表而不塞进 m_Gobjects：
    //   * 所有权模型不同。m_Gobjects 是 shared_ptr 且是唯一持有者，而
    //     Instantiate 出来的对象由 ObjectManager::Produce（= new）创建并由
    //     ObjectManager 管。把裸指针包进 shared_ptr 会双重释放 —— 这正是
    //     PrefabUtility.cpp:703 警告过的同一类错配（Apple 上 new 不走 mimalloc，
    //     喂给 mi_free 会在 mi_usable_size 里 EXC_BAD_ACCESS）。
    //     这里只记指针，不拥有。
    //   * 持久化语义不同。m_Gobjects 会被关卡保存遍历（本文件 collectObjects），
    //     进表 = 被写进 .level 文件；运行时生成的子弹 / 特效不该被持久化。
    //
    // 代价：Tick 和 Find 都要多看一张表（见 Level::Tick / FindGObjectByName）。

    /// 登记一个运行时生成的对象。非拥有 —— 调用方保证它在 Destroy 前有效。
    /// 返回 false 表示 id 已被占用（说明调用方没有给它重新分配 GObjectID）。
    bool RegisterRuntimeGObject(GameObject* gobject);

    /// 从表里摘掉（不销毁对象本身）。
    bool UnregisterRuntimeGObject(GObjectID go_id);

    GameObject* FindRuntimeGObject(GObjectID go_id) const;

    const std::unordered_map<GObjectID, GameObject*>& GetRuntimeGObjects() const
    {
        return m_RuntimeGObjects;
    }

    /// 按名字查：先主表（关卡对象），再运行时表（Instantiate 产物）。
    /// O(n)，与 Unity 的 GameObject.Find 同量级 —— 不是给每帧用的。
    GameObject* FindGObjectByName(const eastl::string& name) const;

    /// 请求销毁。真正销毁发生在 FlushPendingDestroys()（帧末），所以脚本在
    /// 任何时候调用都安全 —— 包括在某个对象的 Tick 里销毁另一个对象：立即销毁
    /// 会让 Level::Tick 手里正在用的迭代器 / 指针当场失效。
    void QueueDestroy(GObjectID go_id);

    /// 执行 QueueDestroy 攒下的销毁。由 Level::Tick 末尾调用。
    void FlushPendingDestroys();
    std::weak_ptr<Character> getCurrentActiveCharacter() const { return m_CurrentActiveCharacter; }
    CameraComponent* GetMainCameraComponent() const;
    void SetMainCamera(GObjectID go_id);

    GObjectID CreateObject(const GameObject& object_instance_res);
    void DeleteGObjectByID(GObjectID go_id);

    /// Editor undo: put back a GameObject that DeleteGObjectByID removed, keeping its
    /// original id and its live component instances (UE keeps a deleted actor alive in
    /// the transaction buffer the same way). Fails if the id is already in use. Only
    /// touches the object table -- transform parenting, scene roots and render state
    /// are the caller's job, mirroring DeleteGObjectByID which does not touch them.
    bool RestoreGObject(const std::shared_ptr<GameObject>& gobject);

    /// Changes every time this Level is loaded or unloaded. Editor undo records
    /// refer to objects by id, which is only meaningful within one load; the undo
    /// system drops its history when this value changes under it.
    uint64_t GetLoadSerial() const { return m_LoadSerial; }

    std::weak_ptr<PhysicsScene> getPhysicsScene() const { return m_PhysicsScene; }

    TransformSceneRoots& GetTransformSceneRoots() { return m_TransformSceneRoots; }
    const TransformSceneRoots& GetTransformSceneRoots() const { return m_TransformSceneRoots; }
    void RebuildAllTransformHierarchies();

protected:
    void clear();
    void FlushRenderDeletes();
    void BumpLoadSerial();

    bool m_IsLoaded {false};
    bool m_IsDirty {false};
    uint64_t m_LoadSerial {0};
    eastl::string m_LevelResUrl;

    // all game objects in this level, key: object id, value: object instance
    LevelObjectsMap m_Gobjects;

    // Runtime-spawned objects (Instantiate). Non-owning: ObjectManager owns them.
    // Deliberately NOT part of m_Gobjects -- see the comment on
    // RegisterRuntimeGObject for why.
    std::unordered_map<GObjectID, GameObject*> m_RuntimeGObjects;

    // Destroy requests, drained by FlushPendingDestroys() at end of Tick.
    std::vector<GObjectID> m_PendingDestroys;

    std::shared_ptr<Character> m_CurrentActiveCharacter;
    /// Id of the character's object after DeleteGObjectByID unbound it, so
    /// RestoreGObject can rebind it. Invalid when the character is bound.
    GObjectID m_DetachedCharacterObjectId {k_invalid_gobject_id};

    std::weak_ptr<PhysicsScene> m_PhysicsScene;

    TransformSceneRoots m_TransformSceneRoots;
};