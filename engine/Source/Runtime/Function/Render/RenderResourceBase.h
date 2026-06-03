#pragma once

#include "Runtime/Function/Render/RenderScene.h"
#include "Runtime/Function/Render/RenderSwapContext.h"
#include "Runtime/Function/Render/RenderType.h"

#include <memory>
#include <string>
#include <unordered_map>

class RHI;
class RenderScene;
class RenderCamera;

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

    // TODO: data caching
    std::shared_ptr<TextureData> LoadTextureHDR(eastl::string file, int desired_channels = 4);
    std::shared_ptr<TextureData> LoadTexture(eastl::string file, bool is_srgb = false);
    RenderMeshData LoadMeshData(const MeshSourceDesc& source, AxisAlignedBox& bounding_box);
    RenderMaterialData LoadMaterialData(const MaterialSourceDesc& source);
    AxisAlignedBox GetCachedBoudingBox(const MeshSourceDesc& source) const;

private:
    StaticMeshData LoadStaticMesh(eastl::string mesh_file, AxisAlignedBox& bounding_box);

    std::unordered_map<MeshSourceDesc, AxisAlignedBox> m_BoundingBoxCacheMap;
};