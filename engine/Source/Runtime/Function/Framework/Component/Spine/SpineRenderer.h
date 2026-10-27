#pragma once

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"
#include "Runtime/Function/Render/Spine/SpineGpuRenderer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "SpineRenderer.generated.h"

class GameObject;
class Level;
struct SpineGpuAssetInfo;

/// 面向相机（SpineGpuBillboardMode 的反射版；值一一对应，SpineRenderer.cpp 里有 static_assert）。
ZENUM()
enum class SpineBillboardMode : unsigned char
{
    none,       // 用 GameObject 的旋转：骨架立在局部 XZ 平面
    full,       // 骨架平面 = 相机平面（屏幕对齐）
    vertical,   // 只绕世界 Z 转：人物直立、正对相机水平方向（2.5D 角色常用）
};

// 序列化按底层整数（同 CameraMode）：场景 YAML 里存 0 / 1 / 2
template<>
class SerializeTraits<SpineBillboardMode> : public SerializeTraitsBase<SpineBillboardMode>
{
public:
    using value_type = SpineBillboardMode;

    inline static const char* GetTypeString(void* /*p*/ = nullptr) { return "SpineBillboardMode"; }
    inline static bool AllowTransferOptimization() { return true; }

    template<typename TransferFunction>
    inline static void Transfer(value_type& data, TransferFunction& transfer)
    {
        using UnderlyingType = std::underlying_type_t<value_type>;
        transfer.TransferBasicData(reinterpret_cast<UnderlyingType&>(data));
    }
};

/// 一条动画事件（spine::AnimationState 的 listener 回调同义）。
struct SpineAnimationEvent
{
    /// "start" / "interrupt" / "end" / "dispose" / "complete" / "event"（顺序与参考实现的 listener 完全一致）
    eastl::string type;
    int32_t track = 0;
    eastl::string animation;   // 动画名；空动画（setEmptyAnimation）= "<empty>"
    bool loop = false;
    float track_time = 0.0f;   // 派发那一刻 TrackEntry 的 trackTime
    // ---- 只在 type == "event"（Spine 编辑器里打的用户事件）时有值 = spine::Event ----
    eastl::string name;        // EventData::getName()
    int32_t int_value = 0;
    float float_value = 0.0f;
    eastl::string string_value;
    eastl::string audio_path;  // EventData::getAudioPath()；空 = 没配音频
    float volume = 1.0f;
    float balance = 0.0f;
    float time = 0.0f;         // Event::getTime()：事件在动画里的时刻（秒）
};

/// 场景里的 GPU-Spine 骨架（spine-unity 的 SkeletonAnimation 同位）。
///
/// 资产：`.zspg`（编辑器会把 Assets 下的骨架源自动 cook 成同目录同名 .zspg，见 SpineAutoCook）；也认骨架源文件
/// （`.skel.bytes` / `.skel` / 带同名图集的 `.json`，加载时现烘焙）。引用同时存**路径 + GUID**：路径找不到时按
/// GUID 在 AssetRegistry 里重定向并改写路径（编辑器内重命名 / 移动 .zspg 不断引用）。
///
/// 运行时：解算 / 绘制在 SpineGpuRenderer（渲染线程）。本组件持有一个"世界实例" id，每次 Tick 推变换 + dt，
/// 并取回上一批**动画事件**与轨道快照（相对命令有 1~N 帧延迟，N = 渲染领先逻辑的帧数）。设计约束：
///   * 实例只在 Tick 里创建（不在 PostLoadResource）：放置流程的模板 GameObject 永不 tick，在那建会留孤儿；
///   * 组件 disabled / GameObject 非激活 ⇒ 不 tick ⇒ 渲染器按逻辑帧序号自动不画；
///   * 所属 GameObject 被删 ⇒ 渲染器删实例；撤销复活后下一次 Tick 发现 id 不认识就重建；
///   * 资产被重新 cook（NotifyAssetChanged）⇒ 下一次 Tick 重建实例（动画从头播）。
///
/// 播放 API（C++ 与 TypeScript 同一组，ZFUNCTION）只改运行时状态、**不改序列化字段**：序列化的
/// Animation / Loop / Initial Skin / Time Scale 只在实例创建时与 Inspector 修改时生效。
/// 实例还没建好时（例如 TS 在 OnStart 里调）命令先排队，建好后按顺序补发。
///
/// 朝向：m_Billboard = none 时骨架立在 GameObject 局部 XZ 平面（Z 向上；骨架 x → +X、y → +Z，同 UE
/// Paper2D）；full / vertical 时由渲染线程按当帧相机转（枢轴 = GameObject 位置，GameObject 的旋转不用）。
/// 1 骨架单位 = m_Scale 世界单位（再乘 GameObject 的 lossy scale，x 用 X 轴、y 用 Z 轴，负值 = 镜像）。
/// 深度：画在 RP1 透明 subpass，被场景网格遮挡、不写深度（实例之间远→近排序）；颜色与透明网格一样经 tonemap。
ZCLASS()
class SpineRenderer : public Component
{
    ZGENERATED_BODY(SpineRenderer)

public:
    using EventListener = std::function<void(SpineRenderer&, const SpineAnimationEvent&)>;

    SpineRenderer();
    ~SpineRenderer() override;

    void PostLoadResource(GameObject* parent_object) override;
    void Tick(float delta_time) override;
    void OnSerializedFieldsUpdated() override;
    void SetEnabled(bool enabled) override;
    void OnOwnerActiveChanged(bool active) override;

    /// TS 入口：`SpineRenderer.Of(this.gameObject)`。GameObject::GetComponent 的返回值在 JS 侧的静态类型是
    /// Component（绑定按声明类型发布），看不到本类的方法 ⇒ 需要一个声明为 SpineRenderer* 的入口（同 GetTransform）。
    ZFUNCTION()
    static SpineRenderer* Of(GameObject* game_object);

    // ---- 播放（语义 = spine::AnimationState 同名方法；动画用名字或下标字符串）----
    ZFUNCTION()
    void SetAnimation(int32_t track, const eastl::string& animation, bool loop);
    ZFUNCTION()
    void AddAnimation(int32_t track, const eastl::string& animation, bool loop, float delay);
    ZFUNCTION()
    void SetEmptyAnimation(int32_t track, float mix_duration);
    ZFUNCTION()
    void AddEmptyAnimation(int32_t track, float mix_duration, float delay);
    ZFUNCTION()
    void ClearTrack(int32_t track);
    ZFUNCTION()
    void ClearTracks();
    /// AnimationStateData::setMix（只作用在本实例）
    ZFUNCTION()
    void SetMix(const eastl::string& from, const eastl::string& to, float duration);
    /// Skeleton::setSkin + setSlotsToSetupPose；空串 = 只用 default 皮肤
    ZFUNCTION()
    void SetSkin(const eastl::string& skin);
    ZFUNCTION()
    void SetTimeScale(float time_scale);
    ZFUNCTION()
    float GetTimeScale() const { return m_RuntimeTimeScale; }

    // ---- 状态查询（上一次解算后的快照）----
    /// 该轨道当前动画名；没有 = ""，空动画 = "<empty>"
    ZFUNCTION()
    eastl::string GetCurrentAnimation(int32_t track) const;
    ZFUNCTION()
    float GetCurrentTrackTime(int32_t track) const;

    // ---- 资产元数据 ----
    ZFUNCTION()
    int32_t GetAnimationCount() const;
    ZFUNCTION()
    eastl::string GetAnimationName(int32_t index) const;
    /// 秒；找不到 = -1
    ZFUNCTION()
    float GetAnimationDuration(const eastl::string& animation) const;
    ZFUNCTION()
    int32_t GetSkinCount() const;
    ZFUNCTION()
    eastl::string GetSkinName(int32_t index) const;

    // ---- 事件：本帧取回的那一批（下一次本组件 Tick 前有效；TS 在 OnUpdate 里按下标读）----
    ZFUNCTION()
    int32_t GetEventCount() const { return static_cast<int32_t>(m_FrameEvents.size()); }
    ZFUNCTION()
    eastl::string GetEventType(int32_t index) const;
    ZFUNCTION()
    eastl::string GetEventAnimation(int32_t index) const;
    ZFUNCTION()
    int32_t GetEventTrack(int32_t index) const;
    ZFUNCTION()
    float GetEventTrackTime(int32_t index) const;
    // 用户事件（type == "event"）的值；其它类型的事件上读到的是缺省值（"" / 0 / 1）
    ZFUNCTION()
    eastl::string GetEventName(int32_t index) const;
    ZFUNCTION()
    int32_t GetEventInt(int32_t index) const;
    ZFUNCTION()
    float GetEventFloat(int32_t index) const;
    ZFUNCTION()
    eastl::string GetEventString(int32_t index) const;
    ZFUNCTION()
    eastl::string GetEventAudioPath(int32_t index) const;
    ZFUNCTION()
    float GetEventVolume(int32_t index) const;
    ZFUNCTION()
    float GetEventBalance(int32_t index) const;
    /// 事件在动画里的时刻（Event::getTime）；生命周期事件上 = 0
    ZFUNCTION()
    float GetEventTime(int32_t index) const;
    /// 该轨道**刚 set / add 进去的那条**的 eventThreshold（spine-unity `entry.EventThreshold = x`）：
    /// 它被混出时，mix < threshold 才继续派发它的用户事件。默认 0 = 一开始混出就不派发
    ZFUNCTION()
    void SetEventThreshold(int32_t track, float threshold);
    // 资产里定义的用户事件（不论哪个动画用到）
    ZFUNCTION()
    int32_t GetUserEventCount() const;
    ZFUNCTION()
    eastl::string GetUserEventName(int32_t index) const;
    /// 不导出：std::vector<SpineAnimationEvent> 没有 Converter（也没有"结构体数组"
    /// 这一层的 marshalling）。脚本读事件请用上面那组 GetEventXxx(index)。
    const std::vector<SpineAnimationEvent>& GetFrameEvents() const { return m_FrameEvents; }
    /// 不导出：std::function 跨语言没有等价物。脚本要接事件就在 OnUpdate 里轮询
    /// GetEventCount() / GetEventType(i)。
    void SetEventListener(EventListener listener) { m_EventListener = std::move(listener); }

    // ---- 资产引用 ----
    //
    // 这一整块一个都不用标 ZFUNCTION：这些 getter / setter 背后就是 ZPROPERTY
    // 字段（m_SkeletonDataAsset / m_AnimationName / m_InitialSkinName /
    // m_Loop / m_TimeScale / m_Scale / m_Billboard），ZHeaderTool 已经把它们
    // 发布成 d.ts 里的同名脚本属性，`spine.scale = 0.02` / `spine.billboard =
    // SpineBillboardMode.Vertical` 直接能写。再补一套 SetScale / GetScale
    // 只是同一份数据两套拼写，纯 API 膨胀。
    //
    // 运行时换动画 / 皮肤请用 SetAnimation() / SetSkin()：那两个只改运行时状态，
    // 不会把场景弄脏；而写序列化字段在编辑器里会丢失（没有 setDirtyFlag）。
    const eastl::string& GetSkeletonDataAsset() const { return m_SkeletonDataAsset; }
    const eastl::string& GetSkeletonDataGuid() const { return m_SkeletonDataGuid; }
    const eastl::string& GetAnimationNameField() const { return m_AnimationName; }
    const eastl::string& GetInitialSkinName() const { return m_InitialSkinName; }
    SpineBillboardMode GetBillboard() const { return m_Billboard; }
    /// 编辑器设置（Inspector / 拖放）：改序列化字段，并按新值同步 GUID。
    /// 导出是因为"换骨架资产"需要重新解析 GUID，不是简单赋值。
    ZFUNCTION()
    void SetSkeletonDataAsset(const eastl::string& asset_path);
    void SetAnimationNameField(const eastl::string& animation) { m_AnimationName = animation; }
    void SetInitialSkinName(const eastl::string& skin) { m_InitialSkinName = skin; }
    void SetBillboard(SpineBillboardMode mode) { m_Billboard = mode; }
    /// 不导出：解析绝对路径是编辑器 / 诊断用的实现细节。
    std::string ResolveSkeletonPath();
    /// 当前资产的元数据（动画 / 皮肤名，按文件修改时间缓存）；没设 / 读失败 ⇒ 带 error 的空表
    std::shared_ptr<const SpineGpuAssetInfo> GetAssetInfo() const;

    /// 渲染器里的实例 id（0 = 还没建）。诊断 / 测试用。
    /// 不导出：渲染器内部句柄，脚本拿到只能做"还没建"这一个判断，而那件事
    /// GetSkeletonDataAsset() 为空已经能表达。
    uint32_t GetInstanceId() const { return m_InstanceId; }

    /// 编辑器自动 cook 重写了某个 .zspg（绝对路径）⇒ 用它的组件下一次 Tick 重建实例。游戏线程调用。
    static void NotifyAssetChanged(const std::string& absolute_path);

    /// 在 `level` 里建一个 GameObject（Transform + SpineRenderer），返回其 id（失败 = k_invalid_gobject_id）。
    /// 编辑器拖放（EditorScenePlacement，外面包撤销事务）与控制台 spine.spawn 共用。
    /// `yaw_radians` 绕世界 Z（骨架平面的法线是局部 ±Y）。
    /// `script`（项目相对的 .ts，如 "Scripts/Hero.ts"）非空 ⇒ 再挂一个 TypeScriptComponent。
    static GObjectID SpawnInLevel(Level* level, const std::string& object_name, const eastl::string& skeleton_asset,
                                  const eastl::string& animation, const Vector3& position, float scale,
                                  float yaw_radians = 0.0f, const std::string& script = std::string());
    /// 控制台 spine.spawn 用：启动参数里的命令执行时关卡往往还没加载 ⇒ 先排队，
    /// WorldManager::Tick 在有当前关卡后调 FlushPendingSpawns 落地。
    static void RequestSpawn(const std::string& skeleton_asset, const std::string& animation, const Vector3& position,
                             float scale, const std::string& script = std::string());
    /// 同上，放在场景相机前方 `distance` 世界单位处（落地时按当时的相机算）。
    static void RequestSpawnInFrontOfCamera(const std::string& skeleton_asset, const std::string& animation,
                                            float distance, float scale, const std::string& script = std::string());
    static void FlushPendingSpawns(Level* level);

private:
    /// 把序列化字段落到渲染器：资产变了 ⇒ 重建实例；否则只补发变化的播放参数。
    /// `allow_create`：只有 Tick 里允许新建实例（见类注释）。
    void SyncInstance(bool allow_create);
    void ReleaseInstance();
    /// 推变换 + dt；渲染器不认识 id（对象被删过）⇒ 重建后再推一次。
    void PushTransform(float delta_time);
    /// 取回本帧事件 + 轨道快照，调 C++ 回调
    void CollectEvents();
    /// 实例已建 ⇒ 立刻发；否则排队，建好后补发
    void Submit(std::function<void(uint32_t id)> command);
    eastl::string AnimationNameOf(uint32_t index) const;

    ZPROPERTY(meta=(DisplayName="Skeleton Data", Tooltip="Baked .zspg path, relative to the project content root or absolute. Skeleton sources with a same-name atlas also load."))
    eastl::string m_SkeletonDataAsset;
    ZPROPERTY(meta=(DisplayName="Skeleton Data GUID", Tooltip="AssetRegistry GUID of the .zspg. Used to find the asset again when the path no longer exists (renamed / moved in the editor)."))
    eastl::string m_SkeletonDataGuid;
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
    ZPROPERTY(meta=(DisplayName="Face Camera", Tooltip="none = use the GameObject rotation; full = parallel to the camera plane; vertical = upright, turning around world Z only."))
    SpineBillboardMode m_Billboard {SpineBillboardMode::full};

    // ---- 不序列化：只对本进程有意义 ----
    uint32_t m_InstanceId {0};
    eastl::string m_AppliedAsset;       // 建实例时的 m_SkeletonDataAsset（变了 ⇒ 重建）
    std::string m_AppliedPathKey;       // 建实例时解析出的绝对路径（小写）：NotifyAssetChanged 按它对号
    uint64_t m_AppliedGeneration {0};
    uint64_t m_SeenChangeSerial {0};
    eastl::string m_AppliedAnimation;
    eastl::string m_AppliedSkin;
    bool m_AppliedLoop {true};
    float m_AppliedTimeScale {1.0f};
    float m_RuntimeTimeScale {1.0f};
    std::vector<std::function<void(uint32_t)>> m_PendingCommands;
    std::vector<SpineAnimationEvent> m_FrameEvents;
    std::vector<SpineGpuTrackSnapshot> m_Tracks;   // 上一次解算后的轨道快照
    EventListener m_EventListener;
};
