#include "GlobalParticle.h"

IMPLEMENT_REGISTER_CLASS(GlobalParticleRes)
IMPLEMENT_OBJECT_SERIALIZE(GlobalParticleRes)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(GlobalParticleRes)

template<typename TransferFunction>
void GlobalParticleRes::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_EmitGap, "emit_gap");
    transfer.Transfer(m_EmitCount, "emit_count");
    transfer.Transfer(m_TimeStep, "time_step");
    transfer.Transfer(m_MaxLife, "max_life");
    transfer.Transfer(m_Gravity, "gravity");
    transfer.Transfer(m_ParticleBillboardTexturePath, "particle_billboard_texture_path");
    transfer.Transfer(m_ZengineLogoTexturePath, "zengine_logo_texture_path");
}