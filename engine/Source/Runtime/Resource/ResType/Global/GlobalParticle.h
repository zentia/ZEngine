#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/Vector3.h"

class GlobalParticleRes : public Object
{
    REGISTER_CLASS(GlobalParticleRes);
    DECLARE_OBJECT_SERIALIZE();

public:
    GlobalParticleRes() = default;

    int m_EmitGap;
    int m_EmitCount;
    float m_TimeStep;
    float m_MaxLife;
    Vector3 m_Gravity;
    eastl::string m_ParticleBillboardTexturePath;
    eastl::string m_ZengineLogoTexturePath;
};