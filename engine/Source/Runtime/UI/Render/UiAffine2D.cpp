#include "Runtime/UI/Render/UiAffine2D.h"

#include "Runtime/UI/Core/UITypes.h"

#include <cmath>

UiAffine2D UiAffine2D::Identity()
{
    return UiAffine2D {};
}

UiAffine2D UiAffine2D::Translation(float x, float y)
{
    UiAffine2D result = Identity();
    result.tx = x;
    result.ty = y;
    return result;
}

UiAffine2D UiAffine2D::Scale(float sx, float sy)
{
    UiAffine2D result = Identity();
    result.m00 = sx;
    result.m11 = sy;
    return result;
}

UiAffine2D UiAffine2D::Rotation(float radians)
{
    UiAffine2D result = Identity();
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    result.m00 = c;
    result.m01 = -s;
    result.m10 = s;
    result.m11 = c;
    return result;
}

UiAffine2D UiAffine2D::FromMatrix4x4(const Matrix4x4& matrix)
{
    UiAffine2D result {};
    result.m00 = matrix.m_Mat[0][0];
    result.m01 = matrix.m_Mat[1][0];
    result.m10 = matrix.m_Mat[0][1];
    result.m11 = matrix.m_Mat[1][1];
    result.tx = matrix.m_Mat[3][0];
    result.ty = matrix.m_Mat[3][1];
    return result;
}

UiAffine2D UiAffine2D::operator*(const UiAffine2D& rhs) const
{
    UiAffine2D result {};
    result.m00 = m00 * rhs.m00 + m01 * rhs.m10;
    result.m01 = m00 * rhs.m01 + m01 * rhs.m11;
    result.m10 = m10 * rhs.m00 + m11 * rhs.m10;
    result.m11 = m10 * rhs.m01 + m11 * rhs.m11;
    result.tx = m00 * rhs.tx + m01 * rhs.ty + tx;
    result.ty = m10 * rhs.tx + m11 * rhs.ty + ty;
    return result;
}

void UiAffine2D::TransformPoint(float x, float y, float& out_x, float& out_y) const
{
    out_x = m00 * x + m01 * y + tx;
    out_y = m10 * x + m11 * y + ty;
}

namespace
{
    float QuaternionToZRotationRadians(const Quaternion& rotation)
    {
        const float x = rotation.x;
        const float y = rotation.y;
        const float z = rotation.z;
        const float w = rotation.w;

        const float sin_z = 2.0f * (w * z + x * y);
        const float cos_z = 1.0f - 2.0f * (y * y + z * z);
        return std::atan2(sin_z, cos_z);
    }
}  // namespace

UiAffine2D BuildWidgetSpaceToScreenAffine(const UIRect& rect,
                                          const Vector2& pivot,
                                          const Vector3& scale,
                                          const Quaternion& rotation)
{
    const float pivot_x = rect.width * pivot.x;
    const float pivot_y = rect.height * pivot.y;
    const float angle = QuaternionToZRotationRadians(rotation);

    const UiAffine2D to_pivot = UiAffine2D::Translation(-pivot_x, -pivot_y);
    const UiAffine2D rot = UiAffine2D::Rotation(angle);
    const UiAffine2D scl = UiAffine2D::Scale(scale.x, scale.y);
    const UiAffine2D from_pivot = UiAffine2D::Translation(rect.x + pivot_x, rect.y + pivot_y);

    return from_pivot * rot * scl * to_pivot;
}
