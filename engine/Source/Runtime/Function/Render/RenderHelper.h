#pragma once

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector4.h"

class RenderScene;
class RenderCamera;

// TODO: support cluster lighting
struct ClusterFrustum
{
    // we don't consider the near and far plane currently
    Vector4 m_PlaneRight;
    Vector4 m_PlaneLeft;
    Vector4 m_PlaneTop;
    Vector4 m_PlaneBottom;
    Vector4 m_PlaneNear;
    Vector4 m_PlaneFar;
};

struct BoundingBox
{
    Vector3 min_bound {std::numeric_limits<float>::max(),
                       std::numeric_limits<float>::max(),
                       std::numeric_limits<float>::max()};
    Vector3 max_bound {std::numeric_limits<float>::min(),
                       std::numeric_limits<float>::min(),
                       std::numeric_limits<float>::min()};

    BoundingBox() {}

    BoundingBox(const Vector3& minv, const Vector3& maxv)
    {
        min_bound = minv;
        max_bound = maxv;
    }

    void Merge(const BoundingBox& rhs)
    {
        min_bound.makeFloor(rhs.min_bound);
        max_bound.makeCeil(rhs.max_bound);
    }

    void Merge(const Vector3& point)
    {
        min_bound.makeFloor(point);
        max_bound.makeCeil(point);
    }
};

struct BoundingSphere
{
    Vector3 m_Center;
    float m_Radius;
};

struct FrustumPoints
{
    Vector3 m_FrustumPoints;
};

ClusterFrustum CreateClusterFrustumFromMatrix(Matrix4x4 mat,
                                              float x_left,
                                              float x_right,
                                              float y_top,
                                              float y_bottom,
                                              float z_near,
                                              float z_far);

bool TiledFrustumIntersectBox(ClusterFrustum const& f, BoundingBox const& b);

BoundingBox BoundingBoxTransform(BoundingBox const& b, Matrix4x4 const& m);

bool BoxIntersectsWithSphere(BoundingBox const& b, BoundingSphere const& s);

Matrix4x4 CalculateDirectionalLightCamera(RenderScene& scene, RenderCamera& camera);