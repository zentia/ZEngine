#include "Runtime/Core/Math/AxisAligned.h"

AxisAlignedBox::AxisAlignedBox(const Vector3& center, const Vector3& half_extent)
{
    Update(center, half_extent);
}

void AxisAlignedBox::Merge(const Vector3& new_point)
{
    m_MinCorner.makeFloor(new_point);
    m_MaxCorner.makeCeil(new_point);

    m_Center = 0.5f * (m_MinCorner + m_MaxCorner);
    m_HalfExtent = m_Center - m_MinCorner;
}

void AxisAlignedBox::Update(const Vector3& center, const Vector3& half_extent)
{
    m_Center = center;
    m_HalfExtent = half_extent;
    m_MinCorner = center - half_extent;
    m_MaxCorner = center + half_extent;
}

bool AxisAlignedBox::IsValid() const
{
    return m_MinCorner.x <= m_MaxCorner.x && m_MinCorner.y <= m_MaxCorner.y && m_MinCorner.z <= m_MaxCorner.z;
}

bool AxisAlignedBox::Contains(const Vector3& point) const
{
    return point.x >= m_MinCorner.x && point.x <= m_MaxCorner.x && point.y >= m_MinCorner.y &&
           point.y <= m_MaxCorner.y && point.z >= m_MinCorner.z && point.z <= m_MaxCorner.z;
}
