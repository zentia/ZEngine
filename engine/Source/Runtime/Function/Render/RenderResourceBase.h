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

    virtual void UpdatePerFrameBuffer(std::shared_ptr<RenderScene> render_scene,
                                      std::shared_ptr<RenderCamera> camera) = 0;

    virtual bool HasValidMesh(size_t mesh_asset_id) const
    {
        (void)mesh_asset_id;
        return false;
    }

    // TODO: data caching
    std::shared_ptr<TextureData> LoadTextureHDR(eastl::string file, int desired_channels = 4);
    std::shared_ptr<TextureData> LoadTexture(eastl::string file, bool is_srgb = false);
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