#pragma once
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Core/Math/Vector4.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

class ParticleComponentRes
{
public:
    DECLARE_SERIALIZE(ParticleComponentRes)

    Vector3 m_LocalTranslation;  // local translation
    Quaternion m_LocalRotation;  // local rotation
    Vector4 m_Velocity;          // velocity base & variance
    Vector4 m_Acceleration;      // acceleration base & variance
    Vector3 m_Size;              // size base & variance
    int m_EmitterType;
    Vector2 m_Life;   // life base & variance
    Vector4 m_Color;  // color rgba
};

template<typename TransferFunction>
void ParticleComponentRes::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_LocalTranslation, "local_translation");
    transfer.Transfer(m_LocalRotation, "local_rotation");
    transfer.Transfer(m_Velocity, "velocity");
    transfer.Transfer(m_Acceleration, "acceleration");
    transfer.Transfer(m_Size, "size");
    transfer.Transfer(m_EmitterType, "emitter_type");
    transfer.Transfer(m_Life, "life");
    transfer.Transfer(m_Color, "color");
}

INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(ParticleComponentRes, , Transfer, void)
