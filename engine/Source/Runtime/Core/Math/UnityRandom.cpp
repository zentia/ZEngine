#include "Runtime/Core/Math/UnityRandom.h"

#include <chrono>
#include <cmath>

// =============================================================================
// PCG32 (xsh-rr 64/32), from the reference implementation.
//
// Chosen over mt19937 for two reasons that matter here (see the header): the
// whole state is one uint64 that can be serialised, and the step is six lines
// of specified integer arithmetic rather than a library implementation detail,
// so the same seed gives the same sequence on every standard library.
//
// The two constants are the ones published with the generator; they are not
// arbitrary tuning knobs and must not be "improved" -- changing them silently
// invalidates every recorded replay.
// =============================================================================

Random::State Random::s_State = 0x853C49E6748FEA9Bull;

namespace
{
    constexpr uint64_t kMultiplier = 0x5851F42D4C957F2Dull;
    constexpr uint64_t kIncrement = 0x14057B7EF767814Full;

    constexpr float kTwoPi = 6.28318530717958647692f;
}  // namespace

uint32_t Random::NextUInt32()
{
    const uint64_t old_state = s_State;
    s_State = old_state * kMultiplier + kIncrement;

    const uint32_t xor_shifted = static_cast<uint32_t>(((old_state >> 18u) ^ old_state) >> 27u);
    const uint32_t rotation = static_cast<uint32_t>(old_state >> 59u);
    // The rotation is masked to 5 bits by the shift above, so the "or" form is
    // safe: `(x >> r) | (x << (32 - r))` would be UB at r == 0.
    return (xor_shifted >> rotation) | (xor_shifted << ((~rotation + 1u) & 31u));
}

void Random::InitState(uint32_t seed)
{
    // The increment is OR'd with 1 because PCG requires an ODD increment; an
    // even one shortens the period. Seeding must scramble the user's small
    // value into the whole state, hence the multiplication, not just
    // `s_State = seed`.
    s_State = (static_cast<uint64_t>(seed) * kMultiplier) | kIncrement | 1ull;
    // Discard one output: adjacent seeds (0, 1, 2, ...) would otherwise produce
    // correlated opening values, which is exactly what someone testing "does
    // seed matter?" would notice.
    NextUInt32();
}

void Random::InitUnseeded()
{
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
    const uint64_t nanos = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    // Mixing in the address of a local is not for security -- it is so two
    // processes started in the same millisecond (CI, a batch launcher, a
    // game launching two clients) do not draw the same sequence.
    const uint64_t address = reinterpret_cast<uintptr_t>(&nanos);
    InitState(static_cast<uint32_t>((nanos ^ (address << 1)) & 0xFFFFFFFFull));
}

Random::State Random::GetState()
{
    return s_State;
}

void Random::SetState(State state)
{
    s_State = state;
}

float Random::value()
{
    // 24 bits of mantissa, not 32: a float cannot hold 32 bits of precision, so
    // asking for more would just fold bits and bias the low end.
    const uint32_t bits = NextUInt32() >> 8;
    return static_cast<float>(bits) / 16777216.0f;  // 2^24
}

float Random::Range(float min, float max)
{
    if (max < min)
    {
        // Unity clamps rather than throwing; swapping silently would be a
        // different distribution than the caller drew from.
        const float t = min;
        min = max;
        max = t;
    }
    return min + (max - min) * value();
}

int Random::Range(int min, int max)
{
    if (min >= max)
    {
        return min;
    }
    const uint32_t span = static_cast<uint32_t>(max - min);
    // Rejection sampling. `NextUInt32() % span` would bias low values whenever
    // span does not divide 2^32 -- invisible by eye, and it is exactly what a
    // loot table's fairness complaint turns out to be.
    const uint32_t limit = 0xFFFFFFFFu - (0xFFFFFFFFu % span);
    uint32_t draw = NextUInt32();
    int guard = 0;
    while (draw >= limit && guard++ < 64)
    {
        draw = NextUInt32();
    }
    return min + static_cast<int>(draw % span);
}

Vector3 Random::insideUnitSphere()
{
    Vector3 point;
    float squared_length;
    int guard = 0;
    do
    {
        point.x = Range(-1.0f, 1.0f);
        point.y = Range(-1.0f, 1.0f);
        point.z = Range(-1.0f, 1.0f);
        squared_length = point.squaredLength();
        ++guard;
    } while ((squared_length > 1.0f || squared_length == 0.0f) && guard < 64);

    // Rejection, not "normalise a random direction and scale by a random
    // radius": that alternative concentrates samples at the centre because
    // r^3 density is not uniform by volume.
    return point;
}

Vector2 Random::insideUnitCircle()
{
    Vector2 point;
    float squared_length;
    int guard = 0;
    do
    {
        point.x = Range(-1.0f, 1.0f);
        point.y = Range(-1.0f, 1.0f);
        squared_length = point.squaredLength();
        ++guard;
    } while ((squared_length > 1.0f || squared_length == 0.0f) && guard < 64);
    return point;
}

Vector3 Random::onUnitSphere()
{
    // Marsaglia's method: uniform over the surface, with no pole clustering.
    // Naive (uniform theta, uniform phi) bunches at the poles, which shows up
    // immediately if this is used to scatter particles on a globe.
    float z;
    float x;
    float y;
    float squared;
    int guard = 0;
    do
    {
        z = Range(-1.0f, 1.0f);
        const float angle = Range(0.0f, kTwoPi);
        const float r = std::sqrt((1.0f - z * z) > 0.0f ? (1.0f - z * z) : 0.0f);
        x = r * std::cos(angle);
        y = r * std::sin(angle);
        squared = x * x + y * y + z * z;
        ++guard;
    } while ((squared == 0.0f) && guard < 64);
    return Vector3(x, y, z);
}

Vector2 Random::onUnitCircle()
{
    const float angle = Range(0.0f, kTwoPi);
    return Vector2(std::cos(angle), std::sin(angle));
}

Quaternion Random::rotation()
{
    // Shoemake's uniform random rotation: three uniform numbers, no rejection,
    // and uniform over SO(3) -- unlike "random Euler angles", which over-samples
    // orientations near the poles of the gimbal.
    const float u1 = value();
    const float u2 = value();
    const float u3 = value();

    const float sqrt_one_minus_u1 = std::sqrt((1.0f - u1) > 0.0f ? (1.0f - u1) : 0.0f);
    const float sqrt_u1 = std::sqrt(u1);

    const float w = sqrt_one_minus_u1 * std::sin(kTwoPi * u2);
    const float x = sqrt_one_minus_u1 * std::cos(kTwoPi * u2);
    const float y = sqrt_u1 * std::sin(kTwoPi * u3);
    const float z = sqrt_u1 * std::cos(kTwoPi * u3);

    return Quaternion(w, x, y, z);
}

bool Random::Chance(float probability)
{
    if (probability <= 0.0f)
    {
        return false;
    }
    if (probability >= 1.0f)
    {
        return true;
    }
    return value() < probability;
}
