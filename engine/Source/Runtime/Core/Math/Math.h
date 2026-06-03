#pragma once

#include "Runtime/Core/Math/Random.h"

#include <algorithm>
#include <cmath>
#include <limits>
#define CMP(x, y) (fabsf(x - y) < FLT_EPSILON * fmaxf(1.0f, fmaxf(fabsf(x), fabsf(y))))

static const float Math_POS_INFINITY = std::numeric_limits<float>::infinity();
static const float Math_NEG_INFINITY = -std::numeric_limits<float>::infinity();
static const float Math_PI = 3.14159265358979323846264338327950288f;
static const float Math_ONE_OVER_PI = 1.0f / Math_PI;
static const float Math_TWO_PI = 2.0f * Math_PI;
static const float Math_HALF_PI = 0.5f * Math_PI;
static const float Math_fDeg2Rad = Math_PI / 180.0f;
static const float Math_fRad2Deg = 180.0f / Math_PI;
static const float Math_LOG2 = log(2.0f);
static const float Math_EPSILON = 1e-6f;

static const float Float_EPSILON = FLT_EPSILON;
static const float Double_EPSILON = DBL_EPSILON;

class Radian;
class Angle;
class Degree;

class Vector2;
class Vector3;
class Vector4;
class Matrix3x3;
class Matrix4x4;
class Quaternion;

class Radian
{
    float m_Rad;

public:
    explicit Radian(float r = 0)
        : m_Rad(r) {}
    explicit Radian(const Degree& d);
    Radian& operator=(float f)
    {
        m_Rad = f;
        return *this;
    }
    Radian& operator=(const Degree& d);

    float valueRadians() const { return m_Rad; }
    float valueDegrees() const;  // see bottom of this file
    float valueAngleUnits() const;

    void SetValue(float f) { m_Rad = f; }

    const Radian& operator+() const { return *this; }
    Radian operator+(const Radian& r) const { return Radian(m_Rad + r.m_Rad); }
    Radian operator+(const Degree& d) const;
    Radian& operator+=(const Radian& r)
    {
        m_Rad += r.m_Rad;
        return *this;
    }
    Radian& operator+=(const Degree& d);
    Radian operator-() const { return Radian(-m_Rad); }
    Radian operator-(const Radian& r) const { return Radian(m_Rad - r.m_Rad); }
    Radian operator-(const Degree& d) const;
    Radian& operator-=(const Radian& r)
    {
        m_Rad -= r.m_Rad;
        return *this;
    }
    Radian& operator-=(const Degree& d);
    Radian operator*(float f) const { return Radian(m_Rad * f); }
    Radian operator*(const Radian& f) const { return Radian(m_Rad * f.m_Rad); }
    Radian& operator*=(float f)
    {
        m_Rad *= f;
        return *this;
    }
    Radian operator/(float f) const { return Radian(m_Rad / f); }
    Radian& operator/=(float f)
    {
        m_Rad /= f;
        return *this;
    }

    bool operator<(const Radian& r) const { return m_Rad < r.m_Rad; }
    bool operator<=(const Radian& r) const { return m_Rad <= r.m_Rad; }
    bool operator==(const Radian& r) const { return m_Rad == r.m_Rad; }
    bool operator!=(const Radian& r) const { return m_Rad != r.m_Rad; }
    bool operator>=(const Radian& r) const { return m_Rad >= r.m_Rad; }
    bool operator>(const Radian& r) const { return m_Rad > r.m_Rad; }
};

/** Wrapper class which indicates a given angle value is in Degrees.
@remarks
    Degree values are interchangeable with Radian values, and conversions
    will be done automatically between them.
*/
class Degree
{
    float m_Deg;  // if you get an error here - make sure to define/typedef 'float' first

public:
    explicit Degree(float d = 0)
        : m_Deg(d) {}
    explicit Degree(const Radian& r)
        : m_Deg(r.valueDegrees()) {}
    Degree& operator=(float f)
    {
        m_Deg = f;
        return *this;
    }
    Degree& operator=(const Degree& d) = default;
    Degree& operator=(const Radian& r)
    {
        m_Deg = r.valueDegrees();
        return *this;
    }

    float valueDegrees() const { return m_Deg; }
    float valueRadians() const;  // see bottom of this file
    float valueAngleUnits() const;

    const Degree& operator+() const { return *this; }
    Degree operator+(const Degree& d) const { return Degree(m_Deg + d.m_Deg); }
    Degree operator+(const Radian& r) const { return Degree(m_Deg + r.valueDegrees()); }
    Degree& operator+=(const Degree& d)
    {
        m_Deg += d.m_Deg;
        return *this;
    }
    Degree& operator+=(const Radian& r)
    {
        m_Deg += r.valueDegrees();
        return *this;
    }
    Degree operator-() const { return Degree(-m_Deg); }
    Degree operator-(const Degree& d) const { return Degree(m_Deg - d.m_Deg); }
    Degree operator-(const Radian& r) const { return Degree(m_Deg - r.valueDegrees()); }
    Degree& operator-=(const Degree& d)
    {
        m_Deg -= d.m_Deg;
        return *this;
    }
    Degree& operator-=(const Radian& r)
    {
        m_Deg -= r.valueDegrees();
        return *this;
    }
    Degree operator*(float f) const { return Degree(m_Deg * f); }
    Degree operator*(const Degree& f) const { return Degree(m_Deg * f.m_Deg); }
    Degree& operator*=(float f)
    {
        m_Deg *= f;
        return *this;
    }
    Degree operator/(float f) const { return Degree(m_Deg / f); }
    Degree& operator/=(float f)
    {
        m_Deg /= f;
        return *this;
    }

    bool operator<(const Degree& d) const { return m_Deg < d.m_Deg; }
    bool operator<=(const Degree& d) const { return m_Deg <= d.m_Deg; }
    bool operator==(const Degree& d) const { return m_Deg == d.m_Deg; }
    bool operator!=(const Degree& d) const { return m_Deg != d.m_Deg; }
    bool operator>=(const Degree& d) const { return m_Deg >= d.m_Deg; }
    bool operator>(const Degree& d) const { return m_Deg > d.m_Deg; }
};

/** Wrapper class which identifies a value as the currently default angle
    type, as defined by Math::setAngleUnit.
@remarks
    Angle values will be automatically converted between radians and degrees,
    as appropriate.
*/
class Angle
{
    float m_Angle;

public:
    explicit Angle(float angle)
        : m_Angle(angle) {}
    Angle() { m_Angle = 0; }

    explicit operator Radian() const;
    explicit operator Degree() const;
};

class Math
{
private:
    enum class AngleUnit
    {
        AU_DEGREE,
        AU_RADIAN
    };

    // angle units used by the api
    static AngleUnit k_AngleUnit;

public:
    Math();

    static float abs(float value) { return std::fabs(value); }
    static bool isNan(float f) { return std::isnan(f); }
    static float Sqr(float value) { return value * value; }
    static float sqrt(float fValue) { return std::sqrt(fValue); }
    static float InvSqrt(float value) { return 1.f / sqrt(value); }
    static bool RealEqual(float a, float b, float tolerance = std::numeric_limits<float>::epsilon());
    static float Clamp(float v, float min, float max) { return std::clamp(v, min, max); }
    static float GetMaxElement(float x, float y, float z) { return std::max({x, y, z}); }

    static float DegreesToRadians(float degrees);
    static float RadiansToDegrees(float radians);
    static float AngleUnitsToRadians(float units);
    static float RadiansToAngleUnits(float radians);
    static float AngleUnitsToDegrees(float units);
    static float DegreesToAngleUnits(float degrees);

    static float sin(const Radian& rad) { return std::sin(rad.valueRadians()); }
    static float sin(float value) { return std::sin(value); }
    static float cos(const Radian& rad) { return std::cos(rad.valueRadians()); }
    static float cos(float value) { return std::cos(value); }
    static float tan(const Radian& rad) { return std::tan(rad.valueRadians()); }
    static float tan(float value) { return std::tan(value); }
    static Radian acos(float value);
    static Radian asin(float value);
    static Radian atan(float value) { return Radian(std::atan(value)); }
    static Radian atan2(float y_v, float x_v) { return Radian(std::atan2(y_v, x_v)); }

    template<class T>
    static constexpr T max(const T A, const T B)
    {
        return std::max(A, B);
    }

    template<class T>
    static constexpr T min(const T A, const T B)
    {
        return std::min(A, B);
    }

    template<class T>
    static constexpr T max3(const T A, const T B, const T C)
    {
        return std::max({A, B, C});
    }

    template<class T>
    static constexpr T min3(const T A, const T B, const T C)
    {
        return std::min({A, B, C});
    }

    static Matrix4x4
    MakeViewMatrix(const Vector3& position, const Quaternion& orientation, const Matrix4x4* reflect_matrix = nullptr);

    static Matrix4x4
    MakeLookAtMatrix(const Vector3& eye_position, const Vector3& target_position, const Vector3& up_dir);

    static Matrix4x4 MakePerspectiveMatrix(Radian fovy, float aspect, float znear, float zfar);

    static Matrix4x4
    MakeOrthographicProjectionMatrix(float left, float right, float bottom, float top, float znear, float zfar);

    static Matrix4x4
    MakeOrthographicProjectionMatrix01(float left, float right, float bottom, float top, float znear, float zfar);
};

// these functions could not be defined within the class definition of class
// Radian because they required class Degree to be defined
inline Radian::Radian(const Degree& d)
    : m_Rad(d.valueRadians()) {}
inline Radian& Radian::operator=(const Degree& d)
{
    m_Rad = d.valueRadians();
    return *this;
}
inline Radian Radian::operator+(const Degree& d) const
{
    return Radian(m_Rad + d.valueRadians());
}
inline Radian& Radian::operator+=(const Degree& d)
{
    m_Rad += d.valueRadians();
    return *this;
}
inline Radian Radian::operator-(const Degree& d) const
{
    return Radian(m_Rad - d.valueRadians());
}
inline Radian& Radian::operator-=(const Degree& d)
{
    m_Rad -= d.valueRadians();
    return *this;
}

inline float Radian::valueDegrees() const
{
    return Math::RadiansToDegrees(m_Rad);
}

inline float Radian::valueAngleUnits() const
{
    return Math::RadiansToAngleUnits(m_Rad);
}

inline float Degree::valueRadians() const
{
    return Math::DegreesToRadians(m_Deg);
}

inline float Degree::valueAngleUnits() const
{
    return Math::DegreesToAngleUnits(m_Deg);
}

inline Angle::operator Radian() const
{
    return Radian(Math::AngleUnitsToRadians(m_Angle));
}

inline Angle::operator Degree() const
{
    return Degree(Math::AngleUnitsToDegrees(m_Angle));
}

inline Radian operator*(float a, const Radian& b)
{
    return Radian(a * b.valueRadians());
}

inline Radian operator/(float a, const Radian& b)
{
    return Radian(a / b.valueRadians());
}

inline Degree operator*(float a, const Degree& b)
{
    return Degree(a * b.valueDegrees());
}

inline Degree operator/(float a, const Degree& b)
{
    return Degree(a / b.valueDegrees());
}