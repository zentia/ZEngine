#include "Runtime/Function/Framework/Component/Particle/ParticleGpuEmitter.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Render/Particle/ParticleGpuRenderer.h"

#include "core/Log/LogSystem.h"

ParticleGpuEmitter::ParticleGpuEmitter() = default;

ParticleGpuEmitter::~ParticleGpuEmitter()
{
    ReleaseEmitter();
}

void ParticleGpuEmitter::PostLoadResource(GameObject* parent_object)
{
    m_ParentObject = parent_object;
    m_RuntimeTimeScale = m_TimeScale;
    m_Playing = m_PlayOnAwake;
    // 实例不在这里建：见头文件类注释（模板 GameObject 上的组件永远不 tick）
}

void ParticleGpuEmitter::Tick(float delta_time)
{
    SyncEmitter(/*allow_create=*/true);
    PushTransform(delta_time);
}

void ParticleGpuEmitter::OnSerializedFieldsUpdated()
{
    SyncEmitter(/*allow_create=*/false);
    PushTransform(0.0f);
}

void ParticleGpuEmitter::SetEnabled(bool enabled)
{
    Component::SetEnabled(enabled);
    // 停用时保留实例（曲线块已上传），只是不画 —— 与 Spine 那边的"可见"同款处理
    if (m_EmitterId != 0)
    {
        ParticleGpuRenderer::Get().RequestSetVisible(m_EmitterId, enabled && m_Playing);
    }
}

void ParticleGpuEmitter::OnOwnerActiveChanged(bool /*active*/)
{
    PushTransform(0.0f);   // 可见性由 PushTransform 统一按 IsActiveInHierarchy 判定
}

ParticleGpuEmitter* ParticleGpuEmitter::Of(GameObject* game_object)
{
    if (game_object == nullptr)
    {
        return nullptr;
    }
    return game_object->tryGetComponent(ParticleGpuEmitter);
}

void ParticleGpuEmitter::Play()
{
    m_Playing = true;
    if (m_EmitterId != 0)
    {
        ParticleGpuRenderer::Get().RequestSetVisible(m_EmitterId, true);
    }
}

void ParticleGpuEmitter::Stop()
{
    m_Playing = false;
    if (m_EmitterId != 0)
    {
        ParticleGpuRenderer::Get().RequestSetVisible(m_EmitterId, false);
    }
}

void ParticleGpuEmitter::SetTimeScale(float time_scale)
{
    m_TimeScale = time_scale;
    m_RuntimeTimeScale = time_scale;
}

void ParticleGpuEmitter::SetSource(const ParticleGpuEmitterSource& source)
{
    m_Source = source;
    m_HasSource = true;
    // 换了 source 等于换了发射器：丢掉旧实例，下一次 Tick 按新的建。
    // 与 SpineRenderer"换了骨架资产就重建"同一条规则。
    ReleaseEmitter();
    m_AppliedAsset.clear();
}

void ParticleGpuEmitter::ReleaseEmitter()
{
    if (m_EmitterId != 0)
    {
        ParticleGpuRenderer::Get().RequestRemoveEmitter(m_EmitterId);
        m_EmitterId = 0;
    }
}

void ParticleGpuEmitter::SyncEmitter(bool allow_create)
{
    if (m_EmitterId != 0)
    {
        return;
    }
    if (!allow_create || !m_HasSource || m_ParentObject == nullptr)
    {
        return;
    }
    ParticleGpuRenderer& renderer = ParticleGpuRenderer::Get();
    m_EmitterId = renderer.RequestAddEmitter(m_Source);
    if (m_EmitterId == 0)
    {
        LOG_ERROR(ZRender, "ParticleGpuEmitter: RequestAddEmitter 失败（capacity={}）—— 见上一条容量校验日志",
                  m_Source.desc.capacity);
        return;
    }
    m_AppliedAsset = m_EmitterAsset;
    renderer.RequestSetVisible(m_EmitterId, m_Playing);
    LOG_INFO(ZRender, "ParticleGpuEmitter: '{}' -> emitter #{} (capacity={})", m_ParentObject->GetName().c_str(),
             m_EmitterId, m_Source.desc.capacity);
}

void ParticleGpuEmitter::PushTransform(float delta_time)
{
    if (m_EmitterId == 0 || m_ParentObject == nullptr)
    {
        return;
    }
    // 列主序、列向量约定（= ParticleGpuEmulateInput::model 与 SpineRenderer 推给
    // SpineGpuRenderer 的那一坨 &model.m_Mat[0][0] 同款）
    Matrix4x4 model = Matrix4x4::IDENTITY;
    bool active = true;
    if (Transform* transform = m_ParentObject->tryGetComponent(Transform))
    {
        model = transform->GetLocalToWorldMatrix();
        active = m_ParentObject->IsActiveInHierarchy();
    }
    // 粒子是公告板：朝向由渲染线程按相机算（cam_right / cam_up），这里只给"发射器
    // 原点在哪"，所以不像 Spine 那样再叠一个 Rx(+90°) 与缩放。
    const bool visible = active && isEnabled() && m_Playing;
    ParticleGpuRenderer& renderer = ParticleGpuRenderer::Get();
    renderer.RequestSetVisible(m_EmitterId, visible);
    if (!renderer.RequestSetTransform(m_EmitterId, &model.m_Mat[0][0], delta_time * m_RuntimeTimeScale))
    {
        // 渲染器已不认识这个 id ⇒ 所属对象被删过，现在又在 tick（撤销复活）。
        // 丢掉旧 id 重建（不发 remove：渲染器那边已经删了）。
        m_EmitterId = 0;
        SyncEmitter(/*allow_create=*/true);
        if (m_EmitterId != 0)
        {
            renderer.RequestSetVisible(m_EmitterId, visible);
            renderer.RequestSetTransform(m_EmitterId, &model.m_Mat[0][0], delta_time * m_RuntimeTimeScale);
        }
    }
}
