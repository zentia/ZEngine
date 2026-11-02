#pragma once

// =====================================================================
// 场景里的 GPU 粒子发射器（Unity 的 ParticleSystem 组件同位）
// ---------------------------------------------------------------------
// 分工与 SpineRenderer ⇄ SpineGpuRenderer 完全一致：
//   * 本组件只持**一个发射器 id** + 作者数据（容器路径 / GUID / 播放参数）；
//   * 出生 / 解算 / 绘制全在 ParticleGpuRenderer（渲染线程）；
//   * 每次 Tick 推一次变换 + dt。
//
// 生命周期约束（照抄 SpineRenderer 的三条，理由一样）：
//   * 实例只在 Tick 里建（不在 PostLoadResource）：模板 GameObject 永不 tick，
//     在那建会留孤儿；
//   * 组件 disabled / 父级停用 ⇒ RequestSetVisible(false)：渲染器保留缓冲、
//     跳过绘制（不是删实例 —— 复活时不必重建曲线块）；
//   * GameObject 被删 ⇒ 渲染器删实例；撤销复活后下一次 Tick 发现 id 不认识就重建。
//
// 数据来源：容器加载器就绪前走 SetSource() 直接注入 ParticleGpuEmitterSource
// （也是第 4 步 CPU/GPU 对拍拿数据的口子）；加载器来了之后由 m_EmitterAsset /
// m_EmitterGuid 解析（路径失效按 GUID 在 AssetRegistry 重定向，与 Spine 同款）。
// =====================================================================

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"
#include "Runtime/Function/Render/Particle/ParticleGpuRenderer.h"

#include <cstdint>

#include "ParticleGpuEmitter.generated.h"

class GameObject;
class Level;
class Transform;

ZCLASS()
class ParticleGpuEmitter : public Component
{
    ZGENERATED_BODY(ParticleGpuEmitter)

public:
    ParticleGpuEmitter();
    ~ParticleGpuEmitter() override;

    void PostLoadResource(GameObject* parent_object) override;
    void Tick(float delta_time) override;
    void OnSerializedFieldsUpdated() override;
    void SetEnabled(bool enabled) override;
    void OnOwnerActiveChanged(bool active) override;

    /// TS 入口：`ParticleGpuEmitter.Of(this.gameObject)`（同 SpineRenderer::Of 的理由）。
    ZFUNCTION()
    static ParticleGpuEmitter* Of(GameObject* game_object);

    // ---- 播放 ----
    ZFUNCTION()
    void Play();
    ZFUNCTION()
    void Stop();
    ZFUNCTION()
    bool IsPlaying() const { return m_Playing; }
    ZFUNCTION()
    void SetTimeScale(float time_scale);
    ZFUNCTION()
    float GetTimeScale() const { return m_RuntimeTimeScale; }

    // ---- 资产引用（背后就是 ZPROPERTY 字段，不需要再写 getter / setter）----
    const eastl::string& GetEmitterAsset() const { return m_EmitterAsset; }
    const eastl::string& GetEmitterGuid() const { return m_EmitterGuid; }

    /// 不导出：ParticleGpuEmitterSource 带 std::vector，脚本侧没有等价物。
    /// 直接喂一份烘焙产物（容器加载器就绪前的主力入口，也是对拍测试的数据来源）。
    void SetSource(const ParticleGpuEmitterSource& source);
    const ParticleGpuEmitterSource& GetSource() const { return m_Source; }

    /// 渲染器里的发射器 id（0 = 还没建）。诊断 / 测试用。
    uint32_t GetEmitterId() const { return m_EmitterId; }

private:
    /// source / 资产变了 ⇒ 重建；否则只推变化。allow_create：只有 Tick 里允许新建。
    void SyncEmitter(bool allow_create);
    void ReleaseEmitter();
    /// 推变换 + dt；渲染器不认识 id（对象被删过）⇒ 重建后再推一次。
    void PushTransform(float delta_time);

    ZPROPERTY(meta = (DisplayName = "Emitter Asset",
                      Tooltip = "Baked GPU-particle container (.zpgp), relative to the project content root or absolute."))
    eastl::string m_EmitterAsset;
    ZPROPERTY(meta = (DisplayName = "Emitter Asset GUID",
                      Tooltip = "AssetRegistry GUID of the container. Used to find it again when the path no longer exists."))
    eastl::string m_EmitterGuid;
    ZPROPERTY(meta = (DisplayName = "Play On Awake"))
    bool m_PlayOnAwake {true};
    ZPROPERTY(meta = (DisplayName = "Time Scale", ClampMin = 0))
    float m_TimeScale {1.0f};

    // ---- 不序列化 ----
    ParticleGpuEmitterSource m_Source;
    bool m_HasSource {false};
    uint32_t m_EmitterId {0};
    eastl::string m_AppliedAsset;       // 建实例时的 m_EmitterAsset（变了 ⇒ 重建）
    float m_RuntimeTimeScale {1.0f};
    bool m_Playing {false};
};
