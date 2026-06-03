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