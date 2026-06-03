#pragma once

#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector4.h"

#include <limits>

// max particle pool size
static constexpr int s_MaxParticles {300000};
static constexpr int s_DefaultParticleEmitGap {10};
static constexpr int s_DefaultParticleEmitCount {100000};
static constexpr int s_DefaultParticleLifeTime {10};
static constexpr float s_DefaultParticleTimeStep {0.004};

static const Vector4 s_DefaultEmiterPosition {5.71, 13.53, 3.0, 0.5};
static const Vector4 s_DefaultEmiterVelocity {0.02, 0.02, 2.5, 4.0};
static const Vector4 s_DefaultEmiterAcceleration {0.00, 0.00, -2.5, 0.0};
static const Vector3 s_DefaultEmiterSize {0.02, 0.02, 0.0};
static const Vector2 s_DefaultEmiterLife {1.2, 0.0};

enum class EMITTER_TYPE
{
    POINT = 0,
    MESH,
    INVALID
};