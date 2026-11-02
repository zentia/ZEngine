#include "Runtime/Function/Framework/Component/Particle/ParticleComponent.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Particle/ParticleManager.h"
#include "Runtime/Function/Render/RenderSwapContext.h"
#include "Runtime/Function/Render/RenderSystem.h"


namespace Runtime
{

    void ParticleComponent::PostLoadResource(GameObject* parent_object)
    {
        m_ParentObject = parent_object;

        m_LocalTransform.MakeTransform(
            m_ParticleRes.m_LocalTranslation, Vector3::UNIT_SCALE, m_ParticleRes.m_LocalRotation);
        ComputeGlobalTransform();

        // Without a ParticleManager (editor tools, headless tests) there is nowhere to
        // create the emitter. The component still loads; it just has no emitter -- the
        // same state it is in when the manager exists but emitter creation fails.
        if (ParticleManager* particle_manager = GET_SYSTEM(ParticleManager))
        {
            particle_manager->CreateParticleEmitter(m_ParticleRes, m_TransformDesc);
        }
    }

    void ParticleComponent::ComputeGlobalTransform()
    {
        Transform* transform_component =
            m_ParentObject->tryGetComponent<Transform>("Transform");

        Matrix4x4 global_transform_matrix = transform_component->GetLocalToWorldMatrix() * m_LocalTransform;

        Vector3 position, scale;
        Quaternion rotation;

        global_transform_matrix.Decomposition(position, scale, rotation);

        memcpy(m_TransformDesc.m_Position.ptr(), position.ptr(), sizeof(float) * 3);
        rotation.ToRotationMatrix(m_TransformDesc.m_Rotation);
    }

    void ParticleComponent::Play()
    {
        m_PlayState = ParticlePlayState::Playing;
    }

    void ParticleComponent::Stop()
    {
        m_PlayState = ParticlePlayState::Stopped;
    }

    void ParticleComponent::Pause()
    {
        // 已停止的系统不接受暂停（见头文件注释）。
        if (m_PlayState == ParticlePlayState::Playing)
        {
            m_PlayState = ParticlePlayState::Paused;
        }
    }

    void ParticleComponent::Clear()
    {
        m_PlayState = ParticlePlayState::Stopped;
        m_Time      = 0.0f;
    }

    void ParticleComponent::SetPlayState(ParticlePlayState state)
    {
        switch (state)
        {
            case ParticlePlayState::Stopped:
            case ParticlePlayState::Playing:
            case ParticlePlayState::Paused:
                m_PlayState = state;
                break;
            default:
                m_PlayState = ParticlePlayState::Stopped;
                break;
        }
    }

    void ParticleComponent::Tick(float delta_time)
    {
        // Stopped：完全不参与。连 transform 更新也跳过 —— 停止的发射器不需要
        // 知道自己位置变了。
        if (m_PlayState == ParticlePlayState::Stopped)
        {
            return;
        }

        if (m_PlayState == ParticlePlayState::Playing)
        {
            m_Time += delta_time > 0.0f ? delta_time : 0.0f;
        }

        // Emitters are only ever ticked through the render thread's swap data: with no
        // RenderSystem there is nothing to hand the tick to.
        RenderSystem* render_system = GET_SYSTEM(RenderSystem);
        if (render_system == nullptr)
        {
            return;
        }
        // 没挂到对象上（还没 PostLoadResource，或者单测里直接构造出来的）：下面
        // `m_ParentObject->tryGetComponent(Transform)` 会解引用空指针。以前这条
        // 走不到，是因为 Tick 只在有 RenderSystem 时才碰到那一行 —— 一旦
        // RenderSystem 注册进来，一个裸组件就能把进程带下去。
        //
        // ⚠️ 判空放在**时钟之后**：播放状态与时钟不需要父对象，把它们也挡在
        // 后面的话，"单测里造一个裸组件推进时钟"就永远推进不了 —— 而那正是
        // 唯一能在 headless 下验证这条分支的方式。
        if (m_ParentObject == nullptr)
        {
            return;
        }
        RenderSwapContext& swap_context = render_system->GetSwapContext();

        RenderSwapData& logic_swap_data = swap_context.GetLogicSwapData();

        // Paused 时**不**推进模拟（不 AddTick），但照旧同步 transform —— 见
        // 头文件那节注释：暂停中移动发射器，恢复后粒子从新位置继续。
        if (m_PlayState == ParticlePlayState::Playing)
        {
            logic_swap_data.AddTickParticleEmitter(m_TransformDesc.m_Id);
        }

        Transform* transform_component = m_ParentObject->tryGetComponent(Transform);
        if (transform_component->IsDirty())
        {
            ComputeGlobalTransform();

            logic_swap_data.UpdateParticleTransform(m_TransformDesc);
        }
    }
};  // namespace Runtime