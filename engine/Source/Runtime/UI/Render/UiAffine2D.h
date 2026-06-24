#pragma once

#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector2.h"

// 2D affine transform for screen-space UI (top-left origin, +Y down).
// Maps (x, y) as: x' = m00*x + m01*y + tx; y' = m10*x + m11*y + ty.
struct UIAffine2D
{
    float m00 {1.0f};
    float m01 {0.0f};
    float m10 {0.0f};
    float m11 {1.0f};
    float tx {0.0f};
    float ty {0.0f};

    static UIAffine2D Identity();
    static UIAffine2D Translation(float x, float y);
    static UIAffine2D Scale(float sx, float sy);
    static UIAffine2D Rotation(float radians);
    static UIAffine2D FromMatrix4x4(const Matrix4x4& matrix);

    UIAffine2D operator*(const UIAffine2D& rhs) const;

    void TransformPoint(float x, float y, float& out_x, float& out_y) const;
};

UIAffine2D BuildWidgetSpaceToScreenAffine(const struct UIRect& rect,
                                          const Vector2& pivot,
                                          const Vector3& scale,
                                          const Quaternion& rotation);
