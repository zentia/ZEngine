#pragma once

#include "NaniteHiZ.h"
#include "NaniteTypes.h"
#include "Runtime/Function/Render/RenderCamera.h"

#include <memory>
#include <vector>

class RHI;
class RenderScene;

// 层次Z缓冲（用于遮挡剔除）。定义在 NaniteCullingSystem 之前：后者按值持有它
// （m_HiZ），需要完整类型。
class NaniteHierarchicalZBuffer
{
public:
    NaniteHierarchicalZBuffer();
    ~NaniteHierarchicalZBuffer();

    // 初始化。rhi 保留给未来的 GPU 路径；CPU 路径不用它，宽度/高度决定金字塔尺寸。
    bool Initialize(RHI* rhi, uint32_t width, uint32_t height);

    // 清理
    void clear();

    // 上传一帧深度图（reversed-Z：1=最近、0=最远，行主序，长度 width*height）并重建
    // 金字塔。CPU 路径的权威入口——不碰 GPU。
    void UpdateDepthBuffer(const float* depth_data, uint32_t width, uint32_t height);

    // 构建层次结构（从 m_Pyramid 的 mip0 做 max 归约）。
    void BuildHierarchy();

    // 遮挡测试：把 bounds（经 transform 到世界空间）的 AABB 用 view_projection 投影到
    // 屏幕矩形 + 取物体最近深度，再交给 nanite_hiz::TestOccluded。返回 true = 被遮挡。
    // view_projection = projection * view（与 FrustumCull 一致）。
    bool IsOccluded(const AxisAlignedBox& bounds, const Matrix4x4& transform, const Matrix4x4& view_projection);

    // 金字塔只读访问（供测试 / 调试检查构建结果）。
    const nanite_hiz::HiZPyramid& GetPyramid() const { return m_Pyramid; }

private:
    RHI* m_Rhi = nullptr;  // 保留给未来 GPU 路径；CPU 路径不用。

    uint32_t m_Width = 0;
    uint32_t m_Height = 0;

    // CPU 层次 Z 金字塔（max 归约）。替代原来那组 void* m_MipBuffers。
    nanite_hiz::HiZPyramid m_Pyramid;
};

// 裁剪系统 - 负责视锥裁剪、遮挡剔除等
class NaniteCullingSystem
{
public:
    NaniteCullingSystem();
    ~NaniteCullingSystem();

    // 初始化
    bool Initialize(RHI* rhi, const NaniteConfig& config);

    // 清理
    void clear();

    // 执行裁剪（CPU版本）
    NaniteVisibilityResult CullInstancesCPU(const std::vector<NaniteInstance>& instances,
                                            const std::shared_ptr<RenderCamera>& camera,
                                            const NaniteConfig& config);

    // 执行裁剪（GPU版本）
    NaniteVisibilityResult CullInstancesGPU(const std::vector<NaniteInstance>& instances,
                                            const std::shared_ptr<RenderCamera>& camera,
                                            const NaniteConfig& config);

    // 视锥裁剪。
    //
    // **约定：返回 true = 被剔除**（和 OcclusionCull / DistanceCull 一致）。以前这个
    // 函数返回的是"可见"，同文件里两个 Cull* 是相反的约定，调用点只能靠试出来。
    bool
    FrustumCull(const AxisAlignedBox& bounds, const Matrix4x4& transform, const std::shared_ptr<RenderCamera>& camera);

    // 同一件事，但直接吃 view_projection：不需要相机的路径（以及测试）用它。
    bool FrustumCull(const AxisAlignedBox& bounds, const Matrix4x4& transform, const Matrix4x4& view_projection);

    // 遮挡剔除（基于层次Z缓冲）。view_projection = projection * view（与 FrustumCull
    // 一致）：把 bounds 投影到屏幕矩形 + 取物体最近深度。返回 true = 被剔除。
    // HiZ 为空（还没喂过深度图）时保守地"不剔除"——不知道深度就把实例裁掉比不裁更糟。
    bool OcclusionCull(const AxisAlignedBox& bounds, const Matrix4x4& transform, const Matrix4x4& view_projection);

    // 喂一帧深度图（reversed-Z，行主序，width*height 个 float）重建 HiZ 金字塔。
    // CPU 路径的遮挡剔除依赖它；不喂深度则 OcclusionCull 恒不剔除（保守）。
    void UpdateHiZDepth(const float* depth_data, uint32_t width, uint32_t height);

    // HiZ 是否已就绪（喂过至少一帧深度）。
    bool HasHiZ() const { return m_HiZ.GetPyramid().Valid(); }

    // 距离裁剪。返回 true = 被剔除。
    bool DistanceCull(const Vector3& position, float distance, float max_distance);

    // LOD选择：返回这个集群在当前距离下**应该**用的层级。
    // 层级 0 是根（最粗），越大越细。屏幕误差小 ⇒ 可以粗一档（往 0 走）；
    // 误差大 ⇒ 需要更细（往叶子走）。以前这两个分支写反了，而且读的是成员
    // m_Config 而不是传进来的 config。
    uint8_t SelectLOD(const NaniteCluster& cluster,
                      const Vector3& camera_position,
                      const Matrix4x4& instance_transform,
                      const NaniteConfig& config);

    // 这个叶子集群现在要不要画。判据是"它是否被要求得更细"：SelectLOD 返回的层级
    // >= 自己的层级 ⇒ 还需要这一档；更粗一档就够 ⇒ 丢掉它。
    //
    // 这是**没有网格简化器时唯一站得住的 LOD 手段**：被丢掉的那个簇的尺度在屏幕上
    // 已经亚像素，留下的洞看不见。别指望它减少远处网格的三角形数 —— 那需要简化器。
    bool IsClusterVisible(const NaniteCluster& cluster,
                          const Vector3& camera_position,
                          const Matrix4x4& instance_transform,
                          const NaniteConfig& config);

    // 计算视锥平面（世界空间）。以前是 private 且零调用者 —— 视锥裁剪是个恒返回
    // true 的 TODO，所以从来没人需要它。
    void ExtractFrustumPlanes(const Matrix4x4& view_projection, std::vector<Vector4>& out_planes);

    // 点与视锥的关系：1 = 在内部，-1 = 在外部
    int PointFrustumTest(const Vector3& point, const std::vector<Vector4>& planes);

    // AABB与视锥的关系：1 = 完全在内部，0 = 相交，-1 = 完全在外部
    int AabbFrustumTest(const AxisAlignedBox& aabb, const Matrix4x4& transform, const std::vector<Vector4>& planes);

private:
    RHI* m_Rhi;
    NaniteConfig m_Config;

    // CPU 层次 Z 缓冲（遮挡剔除）。深度图经 UpdateHiZDepth 喂入并重建金字塔。
    NaniteHierarchicalZBuffer m_HiZ;

    // GPU裁剪资源（未来路径，暂未使用）
    void* m_CullingBuffer = nullptr;
    void* m_VisibilityBuffer = nullptr;
};