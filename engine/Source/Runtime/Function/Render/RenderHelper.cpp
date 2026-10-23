#include "Runtime/Function/Render/RenderHelper.h"

#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderCommon.h"
#include "Runtime/Function/Render/RenderScene.h"

#include <cfloat>
#include <cmath>
#include <limits>

ClusterFrustum CreateClusterFrustumFromMatrix(Matrix4x4 mat,
                                              float x_left,
                                              float x_right,
                                              float y_top,
                                              float y_bottom,
                                              float z_near,
                                              float z_far)
{
    ClusterFrustum f;

    // the following is in the vulkan space
    // note that the Y axis is flipped in Vulkan
    assert(y_top < y_bottom);

    assert(x_left < x_right);
    assert(z_near < z_far);

    // calculate the tiled frustum
    // [Fast Extraction of Viewing Frustum Planes from the WorldView - Projection
    // Matrix](http://gamedevs.org/uploads/fast-extraction-viewing-frustum-planes-from-world-view-projection-matrix.pdf)

    Matrix4x4 mat_column = mat;

    // [vec.xyz 1][mat.col0] / [vec.xyz 1][mat.col3] > x_right
    // [vec.xyz 1][mat.col0 - mat.col3*x_right] > 0
    f.m_PlaneRight = Vector4(mat_column[0]) - (Vector4(mat_column[3]) * x_right);
    // normalize
    f.m_PlaneRight *= (1.0 / Vector3(f.m_PlaneRight.x, f.m_PlaneRight.y, f.m_PlaneRight.z).length());

    // for example, we try to calculate the "plane_left" of the tile frustum
    // note that we use the row vector to be consistent with the DirectXMath
    // [vec.xyz 1][mat.col0] / [vec.xyz 1][mat.col3] < x_left
    //
    // evidently, the value "[vec.xyz 1][mat.col3]" is expected to be greater than
    // 0 (w of clip space) and we multiply both sides by "[vec.xyz 1][mat.col3]",
    // the inequality symbol remains the same [vec.xyz 1][mat.col0] < [vec.xyz
    // 1][mat.col3]*x_left
    //
    // since "x_left" is a scalar, the "scalar multiplication" is applied
    // [vec.xyz 1][mat.col0] < [vec.xyz 1][mat.col3*x_left]
    // [vec.xyz 1][mat.col0 - mat.col3*x_left] < 0
    //
    // we follow the "DirectX::BoundingFrustum::Intersects", the normal of the
    // plane is pointing ourward [vec.xyz 1][mat.col3*x_left - mat.col0] > 0
    //
    // the plane can be defined as [x y z 1][A B C D] = 0 and the [A B C D] is
    // exactly [mat.col0 - mat.col3*x_left] and we need to normalize the normal[A
    // B C] of the plane let [A B C D] = [mat.col3*x_left - mat.col0] [A B C D] /=
    // length([A B C].xyz)
    f.m_PlaneLeft = (Vector4(mat_column[3]) * x_left) - Vector4(mat_column[0]);
    // normalize
    f.m_PlaneLeft *= (1.0 / Vector3(f.m_PlaneLeft.x, f.m_PlaneLeft.y, f.m_PlaneLeft.z).length());

    // [vec.xyz 1][mat.col1] / [vec.xyz 1][mat.col3] < y_top
    // [vec.xyz 1][mat.col3*y_top - mat.col1] > 0
    f.m_PlaneTop = (Vector4(mat_column[3]) * y_top) - Vector4(mat_column[1]);
    // normalize
    f.m_PlaneTop *= (1.0 / Vector3(f.m_PlaneTop.x, f.m_PlaneTop.y, f.m_PlaneTop.z).length());

    // [vec.xyz 1][mat.col1] / [vec.xyz 1][mat.col3] > y_bottom
    // [vec.xyz 1][mat.col1 - mat.col3*y_bottom] > 0
    f.m_PlaneBottom = Vector4(mat_column[1]) - (Vector4(mat_column[3]) * y_bottom);
    // normalize
    f.m_PlaneBottom *= (1.0 / Vector3(f.m_PlaneBottom.x, f.m_PlaneBottom.y, f.m_PlaneBottom.z).length());

    // [vec.xyz 1][mat.col2] / [vec.xyz 1][mat.col3] < z_near
    // [vec.xyz 1][mat.col3*z_near - mat.col2] > 0
    f.m_PlaneNear = (Vector4(mat_column[3]) * z_near) - Vector4(mat_column[2]);
    f.m_PlaneNear *= (1.0 / Vector3(f.m_PlaneNear.x, f.m_PlaneNear.y, f.m_PlaneNear.z).length());

    // [vec.xyz 1][mat.col2] / [vec.xyz 1][mat.col3] > z_far
    // [vec.xyz 1][mat.col2 - mat.col3*z_far] > 0
    f.m_PlaneFar = Vector4(mat_column[2]) - (Vector4(mat_column[3]) * z_far);
    f.m_PlaneFar *= (1.0 / Vector3(f.m_PlaneFar.x, f.m_PlaneFar.y, f.m_PlaneFar.z).length());

    return f;
}

bool TiledFrustumIntersectBox(ClusterFrustum const& f, BoundingBox const& b)
{
    // we follow the "DirectX::BoundingFrustum::Intersects"

    // Center of the box.
    Vector4 box_center((b.max_bound.x + b.min_bound.x) * 0.5,
                       (b.max_bound.y + b.min_bound.y) * 0.5,
                       (b.max_bound.z + b.min_bound.z) * 0.5,
                       1.0);

    // Distance from the center to each side.
    // half extent //more exactly
    Vector3 box_extents((b.max_bound.x - b.min_bound.x) * 0.5,
                        (b.max_bound.y - b.min_bound.y) * 0.5,
                        (b.max_bound.z - b.min_bound.z) * 0.5);

    // plane_right
    {
        float signed_distance_from_plane_right = f.m_PlaneRight.dotProduct(box_center);
        float radius_project_plane_right =
            Vector3(fabs(f.m_PlaneRight.x), fabs(f.m_PlaneRight.y), fabs(f.m_PlaneRight.z)).dotProduct(box_extents);

        bool intersecting_or_inside_right = signed_distance_from_plane_right < radius_project_plane_right;
        if (!intersecting_or_inside_right)
        {
            return false;
        }
    }

    // plane_left
    {
        float signed_distance_from_plane_left = f.m_PlaneLeft.dotProduct(box_center);
        float radius_project_plane_left =
            Vector3(fabs(f.m_PlaneLeft.x), fabs(f.m_PlaneLeft.y), fabs(f.m_PlaneLeft.z)).dotProduct(box_extents);

        bool intersecting_or_inside_left = signed_distance_from_plane_left < radius_project_plane_left;
        if (!intersecting_or_inside_left)
        {
            return false;
        }
    }

    // plane_top
    {
        float signed_distance_from_plane_top = f.m_PlaneTop.dotProduct(box_center);
        float radius_project_plane_top =
            Vector3(fabs(f.m_PlaneTop.x), fabs(f.m_PlaneTop.y), fabs(f.m_PlaneTop.z)).dotProduct(box_extents);

        bool intersecting_or_inside_top = signed_distance_from_plane_top < radius_project_plane_top;
        if (!intersecting_or_inside_top)
        {
            return false;
        }
    }

    // plane_bottom
    {
        float signed_distance_from_plane_bottom = f.m_PlaneBottom.dotProduct(box_center);
        float radius_project_plane_bottom =
            Vector3(fabs(f.m_PlaneBottom.x), fabs(f.m_PlaneBottom.y), fabs(f.m_PlaneBottom.z))
                .dotProduct(box_extents);

        bool intersecting_or_inside_bottom = signed_distance_from_plane_bottom < radius_project_plane_bottom;
        if (!intersecting_or_inside_bottom)
        {
            return false;
        }
    }

    // plane_near
    {
        float signed_distance_from_plane_near = f.m_PlaneNear.dotProduct(box_center);
        float radius_project_plane_near =
            Vector3(fabs(f.m_PlaneNear.x), fabs(f.m_PlaneNear.y), fabs(f.m_PlaneNear.z)).dotProduct(box_extents);

        bool intersecting_or_inside_near = signed_distance_from_plane_near < radius_project_plane_near;
        if (!intersecting_or_inside_near)
        {
            return false;
        }
    }

    // plane_far
    {
        float signed_distance_from_plane_far = f.m_PlaneFar.dotProduct(box_center);
        float radius_project_plane_far =
            Vector3(fabs(f.m_PlaneFar.x), fabs(f.m_PlaneFar.y), fabs(f.m_PlaneFar.z)).dotProduct(box_extents);

        bool intersecting_or_inside_far = signed_distance_from_plane_far < radius_project_plane_far;
        if (!intersecting_or_inside_far)
        {
            return false;
        }
    }

    return true;
}

BoundingBox BoundingBoxTransform(BoundingBox const& b, Matrix4x4 const& m)
{
    // we follow the "BoundingBox::Transform"

    Vector3 const g_BoxOffset[8] = {Vector3(-1.0f, -1.0f, 1.0f),
                                    Vector3(1.0f, -1.0f, 1.0f),
                                    Vector3(1.0f, 1.0f, 1.0f),
                                    Vector3(-1.0f, 1.0f, 1.0f),
                                    Vector3(-1.0f, -1.0f, -1.0f),
                                    Vector3(1.0f, -1.0f, -1.0f),
                                    Vector3(1.0f, 1.0f, -1.0f),
                                    Vector3(-1.0f, 1.0f, -1.0f)};

    size_t const CORNER_COUNT = 8;

    // Load center and extents.
    // Center of the box.
    Vector3 center((b.max_bound.x + b.min_bound.x) * 0.5,
                   (b.max_bound.y + b.min_bound.y) * 0.5,
                   (b.max_bound.z + b.min_bound.z) * 0.5);

    // Distance from the center to each side.
    // half extent //more exactly
    Vector3 extents((b.max_bound.x - b.min_bound.x) * 0.5,
                    (b.max_bound.y - b.min_bound.y) * 0.5,
                    (b.max_bound.z - b.min_bound.z) * 0.5);

    Vector3 min;
    Vector3 max;

    // Compute and transform the corners and find new min/max bounds.
    for (size_t i = 0; i < CORNER_COUNT; ++i)
    {
        Vector3 corner_before = extents * g_BoxOffset[i] + center;
        Vector4 corner_with_w = m * Vector4(corner_before.x, corner_before.y, corner_before.z, 1.0);
        Vector3 corner = Vector3(
            corner_with_w.x / corner_with_w.w, corner_with_w.y / corner_with_w.w, corner_with_w.z / corner_with_w.w);

        if (0 == i)
        {
            min = corner;
            max = corner;
        }
        else
        {
            min = Vector3(Math::min(min[0], corner[0]), Math::min(min[1], corner[1]), Math::min(min[2], corner[2]));
            max = Vector3(Math::max(max[0], corner[0]), Math::max(max[1], corner[1]), Math::max(max[2], corner[2]));
        }
    }

    BoundingBox b_out;
    b_out.max_bound = max;
    b_out.min_bound = min;
    return b_out;
}

bool BoxIntersectsWithSphere(BoundingBox const& b, BoundingSphere const& s)
{
    for (size_t i = 0; i < 3; ++i)
    {
        if (s.m_Center[i] < b.min_bound[i])
        {
            if ((b.min_bound[i] - s.m_Center[i]) > s.m_Radius)
            {
                return false;
            }
        }
        else if (s.m_Center[i] > b.max_bound[i])
        {
            if ((s.m_Center[i] - b.max_bound[i]) > s.m_Radius)
            {
                return false;
            }
        }
    }

    return true;
}

// 全场景包围盒（世界空间）。CSM 一条路径要建 4 次光空间，如果每次都扫一遍全部
// 实体就是 4N —— 提出来只算一次。
static BoundingBox ComputeSceneBoundingBox(RenderScene& scene)
{
    BoundingBox scene_bounding_box;
    scene_bounding_box.min_bound = Vector3(FLT_MAX, FLT_MAX, FLT_MAX);
    scene_bounding_box.max_bound = Vector3(FLT_MIN, FLT_MIN, FLT_MIN);

    for (const RenderEntity& entity : scene.m_RenderEntities)
    {
        BoundingBox mesh_asset_bounding_box {entity.m_BoundingBox.getMinCorner(),
                                             entity.m_BoundingBox.getMaxCorner()};

        BoundingBox mesh_bounding_box_world = BoundingBoxTransform(mesh_asset_bounding_box, entity.m_ModelMatrix);
        scene_bounding_box.Merge(mesh_bounding_box_world);
    }

    return scene_bounding_box;
}

// 把"相机视锥（或其中一段）在世界空间的包围盒"变成方向光的 正交投影 * 视图。
//
// CSM 和原来那张单张 shadow map 共用这一半：区别只在于传进来的包围盒是整个视锥，
// 还是被切出来的**某一段**。抽出来就是为了让两条路走**同一份**光空间构建逻辑 ——
// 手抄两份迟早会漂，本仓库已经为这种事付出过很多次代价。
static Matrix4x4 BuildDirectionalLightProjViewFromFrustum(RenderScene&       scene,
                                                          const BoundingBox& frustum_bounding_box,
                                                          const BoundingBox& scene_bounding_box)
{
    // CascadedShadowMaps11 / ComputeNearAndFar
    Matrix4x4 light_view;
    Matrix4x4 light_proj;
    {
        Vector3 box_center((frustum_bounding_box.max_bound.x + frustum_bounding_box.min_bound.x) * 0.5,
                           (frustum_bounding_box.max_bound.y + frustum_bounding_box.min_bound.y) * 0.5,
                           (frustum_bounding_box.max_bound.z + frustum_bounding_box.min_bound.z));
        Vector3 box_extents((frustum_bounding_box.max_bound.x - frustum_bounding_box.min_bound.x) * 0.5,
                            (frustum_bounding_box.max_bound.y - frustum_bounding_box.min_bound.y) * 0.5,
                            (frustum_bounding_box.max_bound.z - frustum_bounding_box.min_bound.z) * 0.5);

        Vector3 eye = box_center + scene.m_DirectionalLight.m_Direction * box_extents.length();
        Vector3 center = box_center;
        light_view = Math::MakeLookAtMatrix(eye, center, Vector3(0.0, 0.0, 1.0));

        BoundingBox frustum_bounding_box_light_view = BoundingBoxTransform(frustum_bounding_box, light_view);
        BoundingBox scene_bounding_box_light_view = BoundingBoxTransform(scene_bounding_box, light_view);
        light_proj = Math::MakeOrthographicProjectionMatrix01(
            std::max(frustum_bounding_box_light_view.min_bound.x, scene_bounding_box_light_view.min_bound.x),
            std::min(frustum_bounding_box_light_view.max_bound.x, scene_bounding_box_light_view.max_bound.x),
            std::max(frustum_bounding_box_light_view.min_bound.y, scene_bounding_box_light_view.min_bound.y),
            std::min(frustum_bounding_box_light_view.max_bound.y, scene_bounding_box_light_view.max_bound.y),
            -scene_bounding_box_light_view.max_bound
                 .z,  // the objects which are nearer than the frustum bounding box may caster shadow as well
            -std::max(frustum_bounding_box_light_view.min_bound.z, scene_bounding_box_light_view.min_bound.z));
    }

    return (light_proj * light_view);
}

uint32_t CalculateDirectionalLightCascadeCameras(RenderScene& scene,
                                                 RenderCamera& camera,
                                                 float         split_lambda,
                                                 Matrix4x4*    out_light_proj_view,
                                                 float*        out_split_view_depth,
                                                 uint32_t      max_cascade_count)
{
    if (out_light_proj_view == nullptr || max_cascade_count == 0)
    {
        return 0;
    }

    auto clampf = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
    auto lerp3 = [](const Vector3& a, const Vector3& b, float t)
    {
        return Vector3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
    };

    const Matrix4x4 inverse_proj_view = (camera.GetPersProjMatrix() * camera.GetViewMatrix()).inverse();
    const Vector3   eye_position      = camera.position();
    const Vector3   eye_forward       = camera.forward();
    const BoundingBox scene_bounding_box = ComputeSceneBoundingBox(scene);

    // 本工程的投影矩阵是 01 深度：NDC z=0 是近平面、z=1 是远平面。
    const Vector3 k_ndc_near[4] = {Vector3(-1.0f, -1.0f, 0.0f),
                                   Vector3(1.0f, -1.0f, 0.0f),
                                   Vector3(1.0f, 1.0f, 0.0f),
                                   Vector3(-1.0f, 1.0f, 0.0f)};
    const Vector3 k_ndc_far[4] = {Vector3(-1.0f, -1.0f, 1.0f),
                                  Vector3(1.0f, -1.0f, 1.0f),
                                  Vector3(1.0f, 1.0f, 1.0f),
                                  Vector3(-1.0f, 1.0f, 1.0f)};

    auto unproject = [&inverse_proj_view](const Vector3& ndc)
    {
        const Vector4 p = inverse_proj_view * Vector4(ndc.x, ndc.y, ndc.z, 1.0);
        return Vector3(p.x / p.w, p.y / p.w, p.z / p.w);
    };
    auto view_depth = [&eye_position, &eye_forward](const Vector3& p)
    {
        const Vector3 d = p - eye_position;
        return d.x * eye_forward.x + d.y * eye_forward.y + d.z * eye_forward.z;
    };

    Vector3 near_world[4];
    Vector3 far_world[4];
    float   depth_near = FLT_MAX;
    float   depth_far  = -FLT_MAX;
    for (int i = 0; i < 4; ++i)
    {
        near_world[i] = unproject(k_ndc_near[i]);
        far_world[i]  = unproject(k_ndc_far[i]);
        depth_near    = std::min(depth_near, view_depth(near_world[i]));
        depth_far     = std::max(depth_far, view_depth(far_world[i]));
    }

    // 近平面可能就在相机上（正交相机 / near=0），log 分布会因为除零炸掉。
    depth_near = std::max(depth_near, 0.05f);
    if (!(depth_far > depth_near))
    {
        // 退化：拿不到有效深度范围就退回单张 —— 一级 = 整个视锥，跟老行为一致。
        out_light_proj_view[0] = CalculateDirectionalLightCamera(scene, camera);
        if (out_split_view_depth != nullptr)
        {
            // 全填 +inf：只有一级时不管片元多远，"越过的远平面数"都必须是 0。
            // 留一个真实值给 [0] 反而会出现"深度超过它 -> 选到第 1 级"这种越界。
            for (uint32_t i = 0; i < max_cascade_count; ++i)
            {
                out_split_view_depth[i] = std::numeric_limits<float>::infinity();
            }
        }
        return 1;
    }

    const uint32_t count  = std::min(max_cascade_count, static_cast<uint32_t>(s_MaxCascadeCount));
    const float    lambda = clampf(split_lambda, 0.0f, 1.0f);

    // 每一级占 [prev_t, t] 这一段视锥。t 是在**视图空间深度**上取的比例：透视投影
    // 下视锥的四条棱在世界空间里是直线，所以沿棱做线性插值得到的点与"视图深度线性
    // 增长"是同一件事 —— 这也是为什么 split 距离可以直接拿去跟片元视图深度比。
    float prev_t = 0.0f;
    for (uint32_t i = 0; i < count; ++i)
    {
        const float fraction  = static_cast<float>(i + 1) / static_cast<float>(count);
        const float log_split = depth_near * std::pow(depth_far / depth_near, fraction);
        const float lin_split = depth_near + (depth_far - depth_near) * fraction;
        const float split_depth = lambda * log_split + (1.0f - lambda) * lin_split;

        const float t = clampf((split_depth - depth_near) / (depth_far - depth_near), prev_t, 1.0f);

        BoundingBox cascade_box;
        cascade_box.min_bound = Vector3(FLT_MAX, FLT_MAX, FLT_MAX);
        cascade_box.max_bound = Vector3(FLT_MIN, FLT_MIN, FLT_MIN);
        for (int c = 0; c < 4; ++c)
        {
            cascade_box.Merge(lerp3(near_world[c], far_world[c], prev_t));
            cascade_box.Merge(lerp3(near_world[c], far_world[c], t));
        }

        out_light_proj_view[i] = BuildDirectionalLightProjViewFromFrustum(scene, cascade_box, scene_bounding_box);
        if (out_split_view_depth != nullptr)
        {
            out_split_view_depth[i] = split_depth;
        }
        prev_t = t;
    }

    // 高位补 +inf：级数不足上限时，多出来的槽不能留 0 —— 0 会让"越过的远平面数"
    // 虚高，片元被选到一个根本没画的级上去。
    if (out_split_view_depth != nullptr)
    {
        for (uint32_t i = count; i < max_cascade_count; ++i)
        {
            out_split_view_depth[i] = std::numeric_limits<float>::infinity();
        }
    }

    return count;
}

Matrix4x4 CalculateDirectionalLightCamera(RenderScene& scene, RenderCamera& camera)
{
    Matrix4x4 proj_view_matrix;
    {
        Matrix4x4 view_matrix = camera.GetViewMatrix();
        Matrix4x4 proj_matrix = camera.GetPersProjMatrix();
        proj_view_matrix = proj_matrix * view_matrix;
    }

    BoundingBox frustum_bounding_box;
    // CascadedShadowMaps11 / CreateFrustumPointsFromCascadeInterval
    {
        Vector3 const g_frustum_points_ndc_space[8] = {Vector3(-1.0f, -1.0f, 1.0f),
                                                       Vector3(1.0f, -1.0f, 1.0f),
                                                       Vector3(1.0f, 1.0f, 1.0f),
                                                       Vector3(-1.0f, 1.0f, 1.0f),
                                                       Vector3(-1.0f, -1.0f, 0.0f),
                                                       Vector3(1.0f, -1.0f, 0.0f),
                                                       Vector3(1.0f, 1.0f, 0.0f),
                                                       Vector3(-1.0f, 1.0f, 0.0f)};

        Matrix4x4 inverse_proj_view_matrix = proj_view_matrix.inverse();

        frustum_bounding_box.min_bound = Vector3(FLT_MAX, FLT_MAX, FLT_MAX);
        frustum_bounding_box.max_bound = Vector3(FLT_MIN, FLT_MIN, FLT_MIN);

        size_t const CORNER_COUNT = 8;
        for (size_t i = 0; i < CORNER_COUNT; ++i)
        {
            Vector4 frustum_point_with_w = inverse_proj_view_matrix * Vector4(g_frustum_points_ndc_space[i].x,
                                                                              g_frustum_points_ndc_space[i].y,
                                                                              g_frustum_points_ndc_space[i].z,
                                                                              1.0);
            Vector3 frustum_point = Vector3(frustum_point_with_w.x / frustum_point_with_w.w,
                                            frustum_point_with_w.y / frustum_point_with_w.w,
                                            frustum_point_with_w.z / frustum_point_with_w.w);

            frustum_bounding_box.Merge(frustum_point);
        }
    }

    // CascadedShadowMaps11 / ComputeNearAndFar —— 光空间构建逻辑收敛到
    // BuildDirectionalLightProjViewFromFrustum()，CSM 的每一级也走同一个函数。
    return BuildDirectionalLightProjViewFromFrustum(scene, frustum_bounding_box, ComputeSceneBoundingBox(scene));
}