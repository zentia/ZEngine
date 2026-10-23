#pragma once

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"

#include <cstdint>
#include <string>

#include "SpineRenderer.generated.h"

class Level;

/// 场景里的 GPU-Spine 骨架（spine-unity 的 SkeletonAnimation / UE 的 Component 同位）。
///
/// 资产是 SpineCook 烘焙出来的 `.zspg` 容器（图集页与它同目录）；也认骨架源文件
/// （`.skel.bytes` / `.skel` / `.json` + 同名图集，加载时现烘焙），便于还没 cook 的资产先看效果。
///
/// 运行时实际的解算 / 绘制全在 SpineGpuRenderer（渲染线程）里：本组件只持有一个"世界实例" id，
/// 每次 Tick 把 GameObject 的 local-to-world（× m_Scale）和 dt 推过去。设计约束见
/// SpineGpuRenderer.h 的"世界实例"一节，要点：
///   * 实例只在 Tick 里创建（不在 PostLoadResource）：放置流程会先建一个模板 GameObject 再克隆，
///     模板上的组件不进关卡、永远不 tick，若在 PostLoadResource 里就建实例会留下一个孤儿；
///   * 组件 disabled / GameObject 非激活 ⇒ 不 tick ⇒ 渲染器按逻辑帧序号自动不画；
///   * 所属 GameObject 被删 ⇒ 渲染器删实例；撤销复活后下一次 Tick 发现 id 不认识就重建。
///
/// 朝向：本引擎 Z 向上 ⇒ 骨架立在 GameObject 局部 XZ 平面（骨架 x → +X、y → +Z，同 UE Paper2D），
/// 1 骨架单位 = m_Scale 世界单位。
/// ⚠️ 第一版没有深度：画在场景之上，不被网格遮挡（与控制台 spine.load 同一层）。
ZCLASS()
class SpineRenderer : public Component
{
    ZGENERATED_BODY(SpineRenderer)

public:
    SpineRenderer() = default;
    ~SpineRenderer() override;

    void PostLoadResource(GameObject* parent_object) override;
    void Tick(float delta_time) override;
    void OnSerializedFieldsUpdated() override;
    void SetEnabled(bool enabled) override;

    // C++ 侧播放控制（作用在本组件的实例上；语义 = spine::AnimationState 同名方法，轨道 0 起）
    void SetAnimation(int track, const eastl::string& animation, bool loop);
    void SetTimeScale(float time_scale);
    void SetSkin(const eastl::string& skin);

    const eastl::string& GetSkeletonDataAsset() const { return m_SkeletonDataAsset; }
    void SetSkeletonDataAsset(const eastl::string& asset_path);

    /// 渲染器里的实例 id（0 = 还没建）。诊断 / 测试用。
    uint32_t GetInstanceId() const { return m_InstanceId; }

    /// 在 `level` 里建一个 GameObject（Transform + SpineRenderer），返回其 id（失败 = k_invalid_gobject_id）。
    /// 编辑器拖放（EditorScenePlacement，外面包撤销事务）与控制台 spine.spawn 共用。
    /// `yaw_radians` 绕世界 Z（骨架平面的法线是局部 ±Y）。
    static GObjectID SpawnInLevel(Level* level, const std::string& object_name, const eastl::string& skeleton_asset,
                                  const eastl::string& animation, const Vector3& position, float scale,
                                  float yaw_radians = 0.0f);
    /// 控制台 spine.spawn 用：启动参数里的命令执行时关卡往往还没加载 ⇒ 先排队，
    /// WorldManager::Tick 在有当前关卡后调 FlushPendingSpawns 落地。
    static void RequestSpawn(const std::string& skeleton_asset, const std::string& animation, const Vector3& position,
                             float scale);
    /// 同上，放在场景相机前方 `distance` 世界单位处（落地时按当时的相机算）。
    static void RequestSpawnInFrontOfCamera(const std::string& skeleton_asset, const std::string& animation,
                                            float distance, float scale);
    static void FlushPendingSpawns(Level* level);

private:
    /// 把序列化字段落到渲染器：资产变了 ⇒ 重建实例；否则只补发变化的播放参数。
    /// `allow_create`：只有 Tick 里允许新建实例（见类注释）。
    void SyncInstance(bool allow_create);
    void ReleaseInstance();
    /// 推变换 + dt；渲染器不认识 id（对象被删过）⇒ 重建后再推一次。
    void PushTransform(float delta_time);

    ZPROPERTY(meta=(DisplayName="Skeleton Data", Tooltip="Baked .zspg path, relative to the project content root or absolute. Skeleton sources with a same-name atlas also load."))
    eastl::string m_SkeletonDataAsset;
    ZPROPERTY(meta=(DisplayName="Animation", Tooltip="Animation name or index played on track 0. Empty = the first animation."))
    eastl::string m_AnimationName;
    ZPROPERTY()
    bool m_Loop {true};
    ZPROPERTY(meta=(DisplayName="Initial Skin", Tooltip="Skin name. Empty = default skin only."))
    eastl::string m_InitialSkinName;
    ZPROPERTY(meta=(DisplayName="Time Scale", ClampMin=0))
    float m_TimeScale {1.0f};
    ZPROPERTY(meta=(Tooltip="World units per skeleton unit. spine-unity default is 0.01, i.e. 100 skeleton px per unit.", ClampMin=0))
    float m_Scale {0.01f};

    // ---- 不序列化：只对本进程有意义 ----
    uint32_t m_InstanceId {0};
    eastl::string m_AppliedAsset;
    eastl::string m_AppliedAnimation;
    eastl::string m_AppliedSkin;
    bool m_AppliedLoop {true};
    float m_AppliedTimeScale {1.0f};
};
