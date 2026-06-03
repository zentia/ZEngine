#include "Editor/EditorUI/EditorEulerHint.h"

#include "Runtime/Core/Math/Math.h"

#include <cmath>
#include <unordered_map>

namespace EditorEuler
{
namespace
{
    bool IsSameRotation(const Quaternion& lhs, const Quaternion& rhs)
    {
        const float lhs_length = lhs.length();
        const float rhs_length = rhs.length();
        if (lhs_length <= 0.000001f || rhs_length <= 0.000001f)
            return false;

        const float normalized_dot = std::fabs(lhs.dot(rhs) / (lhs_length * rhs_length));
        return normalized_dot >= 0.99999f;
    }

    struct EulerHintEntry
    {
        Vector3 degrees {Vector3::ZERO};
        Quaternion rotation {Quaternion::IDENTITY};
        bool initialized {false};
    };

    std::unordered_map<const void*, EulerHintEntry>& HintMap()
    {
        static std::unordered_map<const void*, EulerHintEntry> map;
        return map;
    }
}  // namespace

Quaternion MakeQuaternionFromEulerDegrees(const Vector3& degrees)
{
    const float pitch_radians = Math::DegreesToRadians(degrees.x);
    const float roll_radians = Math::DegreesToRadians(degrees.y);
    const float yaw_radians = Math::DegreesToRadians(degrees.z);

    Quaternion rotation;
    rotation.w = Math::cos(pitch_radians / 2.0f) * Math::cos(roll_radians / 2.0f) * Math::cos(yaw_radians / 2.0f) +
                 Math::sin(pitch_radians / 2.0f) * Math::sin(roll_radians / 2.0f) * Math::sin(yaw_radians / 2.0f);
    rotation.x = Math::sin(pitch_radians / 2.0f) * Math::cos(roll_radians / 2.0f) * Math::cos(yaw_radians / 2.0f) -
                 Math::cos(pitch_radians / 2.0f) * Math::sin(roll_radians / 2.0f) * Math::sin(yaw_radians / 2.0f);
    rotation.y = Math::cos(pitch_radians / 2.0f) * Math::sin(roll_radians / 2.0f) * Math::cos(yaw_radians / 2.0f) +
                 Math::sin(pitch_radians / 2.0f) * Math::cos(roll_radians / 2.0f) * Math::sin(yaw_radians / 2.0f);
    rotation.z = Math::cos(pitch_radians / 2.0f) * Math::cos(roll_radians / 2.0f) * Math::sin(yaw_radians / 2.0f) -
                 Math::sin(pitch_radians / 2.0f) * Math::sin(roll_radians / 2.0f) * Math::cos(yaw_radians / 2.0f);
    rotation.normalise();
    return rotation;
}

Vector3 MakeEulerDegreesFromQuaternion(const Quaternion& rotation)
{
    return {rotation.GetPitch(false).valueDegrees(),
            rotation.GetRoll(false).valueDegrees(),
            rotation.GetYaw(false).valueDegrees()};
}

Vector3 GetEulerHint(const void* key, const Quaternion& rotation)
{
    EulerHintEntry& hint = HintMap()[key];
    if (!hint.initialized || !IsSameRotation(hint.rotation, rotation))
    {
        hint.degrees = MakeEulerDegreesFromQuaternion(rotation);
        hint.rotation = rotation;
        hint.initialized = true;
    }
    return hint.degrees;
}

void SetEulerHint(const void* key, const Vector3& degrees, const Quaternion& rotation)
{
    EulerHintEntry& hint = HintMap()[key];
    hint.degrees = degrees;
    hint.rotation = rotation;
    hint.initialized = true;
}
}  // namespace EditorEuler
