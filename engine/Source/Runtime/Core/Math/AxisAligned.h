#pragma once

#include "Runtime/Core/Math/Vector3.h"

#include <limits>

class AxisAlignedBox
{
public:
    AxisAlignedBox() {}
    AxisAlignedBox(const Vector3& center, const Vector3& half_extent);

    void Merge(const Vector3& new_point);
    void Update(const Vector3& center, const Vector3& half_extent);

    const Vector3& getCenter() const { return m_Center; }
    const Vector3& getHalfExtent() const { return m_HalfExtent; }
    const Vector3& getMinCorner() const { return m_MinCorner; }
    const Vector3& getMaxCorner() const { return m_MaxCorner; }

    bool IsValid() const;
    bool Contains(const Vector3& point) const;

private:
    Vector3 m_Center {Vector3::ZERO};
    Vector3 m_HalfExtent {Vector3::ZERO};

    Vector3 m_MinCorner {std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::max()};
    Vector3 m_MaxCorner {-std::numeric_limits<float>::max(),
                         -std::numeric_limits<float>::max(),
                         -std::numeric_limits<float>::max()};
};
