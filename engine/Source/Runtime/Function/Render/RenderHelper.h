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

/// 切出 `max_cascade_count` 个级联，每个算一个方向光的 正交投影*视图。
///
/// 返回实际写出的级联数（拿不到有效深度范围时会退化成 1）。
/// `out_split_view_depth[i]` 是第 i 级远平面到相机、沿视线方向的**视图空间深度**，
/// 采样侧就是拿它跟片元的视图深度比来选级的。
///
/// `split_lambda` 是 log 分布和线性分布的混合系数：0 = 纯线性（每级等宽，近处
/// 分辨率不够），1 = 纯 log（近处极窄、远处极宽，远端会糊）。0.5 附近是常用值。
uint32_t CalculateDirectionalLightCascadeCameras(RenderScene& scene,
                                                 RenderCamera& camera,
                                                 float         split_lambda,
                                                 Matrix4x4*    out_light_proj_view,
                                                 float*        out_split_view_depth,
                                                 uint32_t      max_cascade_count);