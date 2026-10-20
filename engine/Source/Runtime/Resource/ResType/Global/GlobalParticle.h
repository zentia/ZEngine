#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/Vector3.h"
#include "GlobalParticle.generated.h"

ZCLASS()
class GlobalParticleRes : public Object
{
    ZGENERATED_BODY(GlobalParticleRes)

public:
    GlobalParticleRes() = default;

    ZPROPERTY()
    int m_EmitGap;
    ZPROPERTY()
    int m_EmitCount;
    ZPROPERTY()
    float m_TimeStep;
    ZPROPERTY()
    float m_MaxLife;
    ZPROPERTY()
    Vector3 m_Gravity;
    ZPROPERTY()
    eastl::string m_ParticleBillboardTexturePath;
    ZPROPERTY()
    eastl::string m_ZengineLogoTexturePath;
};