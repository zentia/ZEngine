#pragma once

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Render/RenderScene.h"
#include "Runtime/Function/Render/RenderSwapContext.h"
#include "Runtime/Function/Render/RenderType.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class RHI;
class RenderScene;
class RenderCamera;

// CPU-side copy of the geometry of an already-uploaded mesh.
//
// RenderMeshData is handed to UploadGameObjectRenderResource **by value** and dropped
// the moment the GPU buffers are filled -- BufferData frees its malloc in the
// destructor. Anything that still needs vertex positions after that point (Lumen's
// distance field, CPU occlusion, ... ) has to read them back from here, keyed by
// RenderEntity::m_MeshAssetId.
//
// Only positions and indices are retained: 12 + 4 bytes per vertex/index instead of
// the 44-byte MeshVertexDataDefinition, because no CPU consumer needs normals,
// tangents or UVs. Indices are widened from the uint16_t asset form to uint32_t so
// the cache does not silently break when a mesh outgrows 65535 vertices.
struct CpuMeshGeometry
{
    std::vector<Vector3>  positions;  // local space, straight out of the asset
    std::vector<uint32_t> indices;
    AxisAlignedBox        local_bounds;

    bool IsValid() const { return !positions.empty() && indices.size() >= 3; }
};

class RenderResourceBase
{
public:
    virtual ~RenderResourceBase() {}

    virtual void clear() = 0;

    virtual void UploadGlobalRenderResource(RHI* rhi, LevelResourceDesc level_resource_desc) = 0;

    virtual void UploadGameObjectRenderResource(RHI* rhi,
                                                RenderEntity render_entity,
                                                RenderMeshData mesh_data,
                                                RenderMaterialData material_data) = 0;

    virtual void
    UploadGameObjectRenderResource(RHI* rhi, RenderEntity render_entity, RenderMeshData mesh_data) = 0;

    virtual void UploadGameObjectRenderResource(RHI* rhi,
                                                RenderEntity render_entity,
                                                RenderMaterialData material_data) = 0;

    /// 给一个带运行时材质 override 的 part 准备**它自己的** GpuPBRMaterial。
    ///
    /// `render_entity.m_MaterialInstanceId != 0` 时才调。实例是 `m_MaterialAssetId`
    /// 那份共享材质的克隆：CPU 字段、纹理视图、采样器全共享，只有 uniform buffer
    /// 和描述符集是自己的 —— 所以改 baseColor / emissive / roughness 不影响用同一
    /// 个资产的其它 part，而纹理不会为此多占一份显存。
    ///
    /// 默认空实现：别的 RenderResourceBase 实现不接这个功能时，描画会走
    /// `GetEntityMaterial` 的回退（用共享材质），行为与加这个功能之前完全一致。
    virtual void UploadGameObjectMaterialInstance(RHI* rhi, RenderEntity render_entity)
    {
        (void)rhi;
        (void)render_entity;
    }

    virtual void UpdatePerFrameBuffer(std::shared_ptr<RenderScene> render_scene,
                                      std::shared_ptr<RenderCamera> camera) = 0;

    virtual bool HasValidMesh(size_t mesh_asset_id) const
    {
        (void)mesh_asset_id;
        return false;
    }

    // TODO: data caching
    std::shared_ptr<TextureData> LoadTextureHDR(eastl::string file, int desired_channels = 4);
    /// `allow_cooked=false` 强制走源图解码，跳过 `_Generated/Textures/` 里的压缩
    /// `.zasset`。**颜色分级 LUT 必须传 false**：BC7/ASTC 是 4x4 块压缩，每一块
    /// 只有两个端点 —— 查表用的数据被这么一压，分级结果会出现色带和错色，而且
    /// 从日志到画面都不会有任何报错。
    std::shared_ptr<TextureData> LoadTexture(eastl::string file, bool is_srgb = false, bool allow_cooked = true);
    RenderMeshData LoadMeshData(const MeshSourceDesc& source, AxisAlignedBox& bounding_box);
    RenderMaterialData LoadMaterialData(const MaterialSourceDesc& source);
    AxisAlignedBox GetCachedBoudingBox(const MeshSourceDesc& source) const;

    // CPU-side geometry cache (see CpuMeshGeometry above).
    //
    // CacheMeshGeometry is idempotent: the first upload of an asset id wins, later
    // uploads of the same asset id are ignored. Call it every time geometry passes
    // through the upload path -- it costs one map lookup when the asset is known and
    // one copy the very first time.
    void CacheMeshGeometry(size_t mesh_asset_id,
                           const RenderMeshData& mesh_data,
                           const AxisAlignedBox& local_bounds);
    const CpuMeshGeometry* FindMeshGeometry(size_t mesh_asset_id) const;
    void ClearMeshGeometryCache();

private:
    StaticMeshData LoadStaticMesh(eastl::string mesh_file, AxisAlignedBox& bounding_box);

    std::unordered_map<MeshSourceDesc, AxisAlignedBox> m_BoundingBoxCacheMap;
    std::unordered_map<size_t, CpuMeshGeometry> m_MeshGeometryCache;
};