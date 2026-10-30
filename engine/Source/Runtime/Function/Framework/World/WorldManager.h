#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Math/Vector3.h"

#include <EASTL/unordered_map.h>
#include "Runtime/Function/Framework/World/WorldPartition.h"
#include "Runtime/Resource/ResType/Common/World.h"

#include <filesystem>

class Level;
class LevelDebugger;
class PhysicsScene;
struct Vector3;

/// Manage game worlds and levels. Supports optional UE-style World Partition cell streaming.
class WorldManager : public IEngineSystem
{
    friend class WorldPartition;

public:
    std::string GetName() const override { return "WorldManager"; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Gameplay; }

    bool Initialize() override;
    void Shutdown() override;

    void ReloadCurrentLevel();
    void SaveCurrentLevel();
    bool IsCurrentLevelDirty() const;
    void MarkCurrentLevelDirty();
    // Save the active level to a new URL (Unity-style Save Scene As). Updates the
    // loaded-level map when the URL changes.
    bool SaveCurrentLevelAs(const eastl::string& new_level_url);
    // Replace the editor's open scene with level_url (single-scene mode; no WP).
    bool OpenScene(const eastl::string& level_url);

    void Tick(float delta_time);

    /// 每个固定步一次。只做"模拟推进"那一半（组件 FixedUpdate + 物理），
    /// 不做流式加载、不做 GPU-Spine 世界 tick 存活标记 —— 那些是**每帧**的
    /// 事情，跟帧率走是对的。
    void FixedTick(float fixed_delta_time);

    Level* getCurrentActiveLevel() const { return m_CurrentActiveLevel; }

    bool IsWorldLoaded() const { return m_IsWorldLoaded; }
    bool IsWorldPartitionEnabled() const { return m_WorldPartition.IsEnabled(); }
    size_t GetLoadedWorldPartitionCellCount() const { return m_WorldPartition.GetLoadedCellCount(); }
    size_t GetPendingWorldPartitionCellCount() const { return m_WorldPartition.GetPendingCellCount(); }
    const WorldPartition& GetWorldPartition() const { return m_WorldPartition; }
    Vector3 GetWorldPartitionStreamingSource() const;
    eastl::vector<eastl::string> GetLoadedLevelUrls() const;

    std::weak_ptr<PhysicsScene> GetCurrentActivePhysicsScene() const;

    // Used by WorldPartition to stream cells (one Level instance per loaded cell).
    bool LoadLevelAdditive(const eastl::string& level_url);
    void UnloadLevelByUrl(const eastl::string& level_url);
    bool AcquireLevelUrl(const eastl::string& level_url);
    void ReleaseLevelUrl(const eastl::string& level_url);
    void StartPreloadLevelUrl(const eastl::string& level_url);
    bool IsLevelUrlReadyForActivation(const eastl::string& level_url) const;
    void RefreshActiveLevelAfterPartitionUpdate();

    /// Reload the active level from its own URL, discarding any runtime state
    /// (this is what PIE does on stop: the editor world has been mutated by
    /// gameplay, and the only cheap way back to the authored state is to reload).
    /// Returns false when there is no active level or the reload fails.
    bool ReloadActiveLevelInPlace();

    // -------------------------------------------------------------------------
    // PIE（编辑器内播放）—— UE 式独立 Play World
    // -------------------------------------------------------------------------
    // UE 进入 PIE 时把编辑器世界复制成第二个 World，游戏逻辑只改那一份，
    // 停止时直接丢弃，编辑器世界分毫未动，因此停止后无需重载、也不会有
    // "未保存改动"的问题。
    //
    // ZEngine 这里用"临时副本"实现同一个语义：把编辑器关卡当前状态（含未
    // 保存的改动）另存到 <project>/<saved>/PIE/ 下，再用这个不同的路径加载
    // 第二份 Level 并设为活动关卡。之所以走磁盘而不是内存深拷贝，是因为
    // 实例 ID 由 (路径, fileID) 派生（GetInstanceIDFromPathAndFileID），
    // ObjectManager 的 ID 表又是全进程一张表；同一路径加载两次会让第二份
    // 覆盖第一份的表项，两个世界的 PPtr 会互相串。换成不同路径后 ID 天然
    // 不同，隔离自动成立。

    /// 把当前活动关卡另存为 PIE 临时副本并加载成第二个世界，返回其 URL。
    /// 失败返回空串。BeginPIE 调用。
    bool BeginPlayWorld(eastl::string& out_play_world_url);

    /// 卸载 PIE 世界并把编辑器原来的关卡恢复为活动关卡。EndPIE 调用。
    void EndPlayWorld();

    bool IsInPlayWorld() const { return m_IsInPlayWorld; }
    /// PIE 期间编辑器自己那份关卡（EndPlayWorld 要恢复的目标）。
    Level* GetEditorLevel() const { return m_EditorLevel; }

private:
    bool LoadWorld(const eastl::string& world_url);
    bool LoadLevel(const eastl::string& level_url);

    /// Create the rigid bodies that PostLoadResource had to defer because the
    /// level was not yet active. Must run before the deferred navmesh build:
    /// the navmesh raycasts against colliders, so building it first would see
    /// an empty physics scene and produce zero walkable cells.
    void FlushPendingRigidBodies(Level* level);

    void TickLoadedLevels(float delta_time);
    void UpdateWorldPartitionStreaming();

    bool m_IsWorldLoaded {false};
    eastl::string m_CurrentWorldUrl;
    WorldRes* m_CurrentWorldResource {nullptr};

    eastl::unordered_map<eastl::string, Level*> m_LoadedLevels;
    Level* m_CurrentActiveLevel {nullptr};

    // ---- PIE（独立 Play World）状态 ----
    /// 构造 PIE 临时副本的落盘路径：<project>/<saved>/PIE/<stem>_pie.scene。
    eastl::string MakePlayWorldUrl(const eastl::string& editor_level_url) const;

    bool m_IsInPlayWorld {false};
    /// PIE 期间编辑器那份关卡的指针（Play World 是另一个 Level 实例）。
    Level* m_EditorLevel {nullptr};
    /// 编辑器那份关卡的 URL，停止后按它把活动关卡复位回去。
    eastl::string m_EditorLevelUrl;

    WorldPartition m_WorldPartition;
    eastl::unordered_map<eastl::string, int> m_LevelUrlRefCounts;
    std::shared_ptr<LevelDebugger> m_LevelDebugger;
};
