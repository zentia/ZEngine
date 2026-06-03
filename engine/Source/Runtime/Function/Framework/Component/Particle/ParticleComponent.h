#pragma once

#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Transform.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Particle/ParticleDesc.h"
#include "Runtime/Resource/ResType/Components/Emitter.h"

namespace Runtime
{
    class ParticleComponent : public Component
    {
    public:
        ParticleComponent() {}

        void PostLoadResource(GameObject* parent_object) override;

        void Tick(float delta_time) override;

    private:
        void ComputeGlobalTransform();

        ParticleComponentRes m_ParticleRes;

        Matrix4x4 m_LocalTransform;

        ParticleEmitterTransformDesc m_TransformDesc;
    };
}  // namespace Runtime