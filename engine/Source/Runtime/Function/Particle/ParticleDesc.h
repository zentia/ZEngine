#pragma once

#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector4.h"
#include "Runtime/Function/Particle/EmitterIdAllocator.h"
#include "Runtime/Resource/ResType/Components/Emitter.h"

struct ParticleEmitterTransformDesc
{
    ParticleEmitterID m_Id;
    Vector4 m_Position;
    Matrix4x4 m_Rotation;
};

struct ParticleEmitterDesc
{
    Vector4 m_Position;
    Matrix4x4 m_Rotation;
    Vector4 m_Velocity;
    Vector4 m_Acceleration;
    Vector3 m_Size;
    int m_EmitterType;
    Vector2 m_Life;
    Vector2 m_Padding;
    Vector4 m_Color;

    ParticleEmitterDesc() = default;

    ParticleEmitterDesc(const ParticleComponentRes& component_res, ParticleEmitterTransformDesc& transform_desc)
        : m_Position(transform_desc.m_Position), m_Rotation(transform_desc.m_Rotation),
          m_Velocity(component_res.m_Velocity), m_Acceleration(component_res.m_Acceleration),
          m_Size(component_res.m_Size), m_EmitterType(component_res.m_EmitterType), m_Life(component_res.m_Life),
          m_Color(component_res.m_Color)
    {
    }
};