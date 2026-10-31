#include "Runtime/Scripting/ScriptRandom/ScriptRandom.h"

#include "Runtime/Core/Math/UnityRandom.h"

// 纯转发：随机数的一切语义（PCG32、跨机器确定性、int/float 的 max 不对称）
// 都活在 UnityRandom 里。这一层唯一的作用是给它一个脚本能看见的名字 —— 而且
// 那个名字必须叫 `Random`（Unity 的拼写），这才需要把实现改名让位。

void Random::InitState(int32_t seed)
{
    UnityRandom::InitState(static_cast<uint32_t>(seed));
}

void Random::InitUnseeded()
{
    UnityRandom::InitUnseeded();
}

int64_t Random::GetState()
{
    return static_cast<int64_t>(UnityRandom::GetState());
}

void Random::SetState(int64_t state)
{
    UnityRandom::SetState(static_cast<UnityRandom::State>(state));
}

float Random::value()
{
    return UnityRandom::value();
}

int32_t Random::RangeInt(int32_t min, int32_t max)
{
    return UnityRandom::Range(min, max);
}

float Random::RangeFloat(float min, float max)
{
    return UnityRandom::Range(min, max);
}

bool Random::Chance(float probability)
{
    return UnityRandom::Chance(probability);
}

Vector3 Random::insideUnitSphere()
{
    return UnityRandom::insideUnitSphere();
}

Vector3 Random::onUnitSphere()
{
    return UnityRandom::onUnitSphere();
}

Vector2 Random::insideUnitCircle()
{
    return UnityRandom::insideUnitCircle();
}

Vector2 Random::onUnitCircle()
{
    return UnityRandom::onUnitCircle();
}

Quaternion Random::rotation()
{
    return UnityRandom::rotation();
}
