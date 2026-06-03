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

        GET_SYSTEM(ParticleManager)->CreateParticleEmitter(m_ParticleRes, m_TransformDesc);
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

    void ParticleComponent::Tick(float delta_time)
    {
        RenderSwapContext& swap_context = GET_SYSTEM(RenderSystem)->GetSwapContext();

        RenderSwapData& logic_swap_data = swap_context.GetLogicSwapData();

        logic_swap_data.AddTickParticleEmitter(m_TransformDesc.m_Id);

        Transform* transform_component = m_ParentObject->tryGetComponent(Transform);
        if (transform_component->IsDirty())
        {
            ComputeGlobalTransform();

            logic_swap_data.UpdateParticleTransform(m_TransformDesc);
        }
    }
};  // namespace Runtime