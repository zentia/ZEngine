#pragma once

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

#include <cmath>

/// Double-precision 3D vector for absolute world coordinates (UE FVector3d).
class Vector3d
{
public:
    DECLARE_SERIALIZE(Vector3d)

    double x {0.0};
    double y {0.0};
    double z {0.0};

    Vector3d() = default;
    Vector3d(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    explicit Vector3d(const Vector3& v) : x(v.x), y(v.y), z(v.z) {}

    double operator[](size_t i) const
    {
        return *(&x + i);
    }

    double& operator[](size_t i)
    {
        return *(&x + i);
    }

    bool operator==(const Vector3d& rhs) const
    {
        return x == rhs.x && y == rhs.y && z == rhs.z;
    }

    bool operator!=(const Vector3d& rhs) const { return !(*this == rhs); }

    Vector3d operator+(const Vector3d& rhs) const { return Vector3d(x + rhs.x, y + rhs.y, z + rhs.z); }
    Vector3d operator-(const Vector3d& rhs) const { return Vector3d(x - rhs.x, y - rhs.y, z - rhs.z); }
    Vector3d operator*(double scalar) const { return Vector3d(x * scalar, y * scalar, z * scalar); }
    Vector3d operator/(double scalar) const { return Vector3d(x / scalar, y / scalar, z / scalar); }

    Vector3d& operator+=(const Vector3d& rhs)
    {
        x += rhs.x;
        y += rhs.y;
        z += rhs.z;
        return *this;
    }

    Vector3d& operator-=(const Vector3d& rhs)
    {
        x -= rhs.x;
        y -= rhs.y;
        z -= rhs.z;
        return *this;
    }

    Vector3d& operator/=(double scalar)
    {
        x /= scalar;
        y /= scalar;
        z /= scalar;
        return *this;
    }

    double dotProduct(const Vector3d& rhs) const { return x * rhs.x + y * rhs.y + z * rhs.z; }

    double squaredLength() const { return x * x + y * y + z * z; }

    double length() const { return std::sqrt(squaredLength()); }

    void normalise()
    {
        const double len = length();
        if (len > 1e-12)
        {
            *this /= len;
        }
    }

    Vector3 ToVector3() const
    {
        return Vector3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    }

    static const Vector3d ZERO;
};

template<typename TransferFunction>
void Vector3d::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(x, "x");
    transfer.Transfer(y, "y");
    transfer.Transfer(z, "z");
}

inline const Vector3d Vector3d::ZERO {0.0, 0.0, 0.0};
