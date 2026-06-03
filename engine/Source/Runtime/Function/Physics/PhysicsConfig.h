#pragma once

#include "core/Math/Vector3.h"

#include <cstdint>

class PhysicsConfig
{
public:
    // scene setting
    uint32_t m_MaxBodyCount {10240};
    uint32_t m_BodyMutexCount {0};
    uint32_t m_MaxBodyPairs {65536};
    uint32_t m_MaxContactConstraints {10240};

    // job setting
    uint32_t m_MaxJobCount {1024};
    uint32_t m_MaxBarrierCount {8};
    uint32_t m_MaxConcurrentJobCount {4};

    Vector3 m_Gravity {0.f, 0.f, -9.8f};

    float m_UpdateFrequency {60.f};
};