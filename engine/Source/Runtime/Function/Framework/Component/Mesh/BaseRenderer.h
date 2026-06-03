#pragma once

#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Render/RenderObject.h"
#include "Runtime/Resource/ResType/Components/Mesh.h"

#include <vector>

class TransformComponent;

class BaseRenderer : public Component
{
    REGISTER_CLASS(BaseRenderer);
    DECLARE_OBJECT_SERIALIZE();

public:
    BaseRenderer() = default;

    void SetSubMeshes(const std::vector<SubMeshRes>& sub_meshes);
    void SetSharedMaterialAssets(const std::vector<eastl::string>& shared_material_assets);

    // Editor mesh placement / foliage: force no back-face cull even when the bound
    // Material has is_double_sided=false. Not serialized.
    void SetForceDoubleSided(bool force) { m_ForceDoubleSided = force; }
    bool GetForceDoubleSided() const { return m_ForceDoubleSided; }

    const std::vector<SubMeshRes>& getSubMeshes() const { return m_SubMeshes; }
    const std::vector<eastl::string>& getSharedMaterialAssets() const { return m_SharedMaterialAssets; }

    void PostLoadResource(GameObject* parent_object) override;
    void OnSerializedFieldsUpdated() override;
    void Tick(float delta_time) override;

protected:
    std::vector<GameObjectPartDesc> BuildRenderParts(const TransformComponent* transform_component) const;
    GameObjectDesc BuildGameObjectDescFromParts(const std::vector<GameObjectPartDesc>& render_parts) const;
    void SubmitRenderState() const;
    eastl::string ResolveMaterialAsset(size_t sub_mesh_index) const;
    void MigrateLegacyMaterialAssets();

protected:
    virtual GameObjectDesc BuildGameObjectDesc(const TransformComponent* transform_component) const;

private:
    std::vector<SubMeshRes> m_SubMeshes;
    std::vector<eastl::string> m_SharedMaterialAssets;
    bool m_ForceDoubleSided {false};
};
