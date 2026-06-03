#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Vector4.h"
#include "Runtime/Function/Particle/ParticleDesc.h"
#include "Runtime/Resource/ResType/Components/Emitter.h"
#include "Runtime/Resource/ResType/Global/GlobalParticle.h"

#include <memory>

class ParticlePass;
class ParticleManager : public IEngineSystem
{
public:
    std::string GetName() const override { return "ParticleManager"; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Rendering; }

    ParticleManager() = default;

    ~ParticleManager() {};

    void SetParticlePass(ParticlePass* particle_pass);

    GlobalParticleRes* GetGlobalParticleRes();

    void CreateParticleEmitter(const ParticleComponentRes& particle_res, ParticleEmitterTransformDesc& transform_desc);

protected:
    bool Initialize() override;
    void Shutdown() override;

private:
    GlobalParticleRes* m_GlobalParticleRes;
};