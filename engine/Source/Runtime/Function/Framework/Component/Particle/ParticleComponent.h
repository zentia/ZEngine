#pragma once

#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Particle/ParticleDesc.h"
#include "Runtime/Resource/ResType/Components/Emitter.h"
#include "ParticleComponent.generated.h"

namespace Runtime
{
    ZCLASS()
    class ParticleComponent : public Component
    {
        ZGENERATED_BODY(ParticleComponent)

    public:
        ParticleComponent() {}

        void PostLoadResource(GameObject* parent_object) override;

        void Tick(float delta_time) override;

    private:
        void ComputeGlobalTransform();

        ZPROPERTY()
        ParticleComponentRes m_ParticleRes;

        Matrix4x4 m_LocalTransform;

        ParticleEmitterTransformDesc m_TransformDesc;
    };
}  // namespace Runtime