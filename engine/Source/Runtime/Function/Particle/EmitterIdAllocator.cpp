#include "Runtime/Function/Particle/EmitterIdAllocator.h"

#include "core/base/Macro.h"

std::atomic<ParticleEmitterID> ParticleEmitterIDAllocator::m_NextId {0};

ParticleEmitterID ParticleEmitterIDAllocator::Alloc()
{
    std::atomic<ParticleEmitterID> new_emitter_ret = m_NextId.load();
    m_NextId++;
    if (m_NextId >= k_invalid_particke_emmiter_id)
    {
        LOG_FATAL(ZParticle, "particle emitter id overflow");
    }

    return new_emitter_ret;
}

void ParticleEmitterIDAllocator::reset()
{
    m_NextId.store(0);
}