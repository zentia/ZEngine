#include "ParticleManager.h"

#include "Runtime/Function/Particle/EmitterIdAllocator.h"
#include "Runtime/Function/Render/Passes/ParticlePass.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Config/ConfigManager.h"

bool ParticleManager::Initialize()
{
    const eastl::string& global_particle_res_url = GET_SYSTEM(ConfigManager)->GetGlobalParticleResUrl();
    m_GlobalParticleRes = GET_SYSTEM(AssetManager)->loadAsset<GlobalParticleRes>(global_particle_res_url);
    if (!m_GlobalParticleRes)
    {
        LOG_FATAL(ZParticle, "{} load failed!", global_particle_res_url.c_str());
        return false;
    }

    if (m_GlobalParticleRes->m_EmitGap < 0)
    {
        m_GlobalParticleRes->m_EmitGap = s_DefaultParticleEmitGap;
    }
    if (m_GlobalParticleRes->m_EmitGap % 2)
    {
        LOG_ERROR(ZParticle, "emit_gap should be multiples of 2");
        m_GlobalParticleRes->m_EmitGap = s_DefaultParticleEmitGap;
    }
    if (m_GlobalParticleRes->m_TimeStep < 1e-6)
    {
        LOG_ERROR(ZParticle, "time_step should be lager");
        m_GlobalParticleRes->m_EmitGap = s_DefaultParticleTimeStep;
    }
    if (m_GlobalParticleRes->m_MaxLife < m_GlobalParticleRes->m_TimeStep)
    {
        LOG_ERROR(ZParticle, "max_life should be larger");
        m_GlobalParticleRes->m_MaxLife = s_DefaultParticleLifeTime * s_DefaultParticleTimeStep;
    }
    return true;
}

void ParticleManager::Shutdown()
{
    MemoryManager::DestroyObject(m_GlobalParticleRes);
    m_GlobalParticleRes = nullptr;
}

void ParticleManager::CreateParticleEmitter(const ParticleComponentRes& particle_res,
                                            ParticleEmitterTransformDesc& transform_desc)
{
    RenderSwapContext& swap_context = GET_SYSTEM(RenderSystem)->GetSwapContext();
    RenderSwapData& swap_data = swap_context.GetLogicSwapData();

    ParticleEmitterDesc desc(particle_res, transform_desc);
    swap_data.AddNewParticleEmitter(desc);

    transform_desc.m_Id = ParticleEmitterIDAllocator::Alloc();
}

GlobalParticleRes* ParticleManager::GetGlobalParticleRes()
{
    return m_GlobalParticleRes;
}