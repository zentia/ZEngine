#pragma once

#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"
#include "Runtime/Function/Render/MaterialCustomParamSlots.h"
#include "Runtime/Function/ShaderLab/ShaderPropertyBlob.h"

#include <EASTL/vector.h>
#include <vector>

class GameObjectMeshDesc
{
public:
    // Unity MeshFilter.sharedMesh equivalent: mesh asset reference only.
    eastl::string m_MeshAsset;
};

class SkeletonBindingDesc
{
public:
    eastl::string m_SkeletonBindingFile;
};

class SkeletonAnimationResultTransform
{
public:
    Matrix4x4 m_Matrix;
};

class SkeletonAnimationResult
{
public:
    std::vector<SkeletonAnimationResultTransform> m_Transforms;
};

class GameObjectMaterialDesc
{
public:
    // Unity Renderer.sharedMaterials equivalent: material asset reference only.
    eastl::string m_MaterialAsset;
    eastl::string m_Shader {"StandardLit"};

    Vector4 m_BaseColorFactor {1.0f, 1.0f, 1.0f, 1.0f};
    float m_MetallicFactor {1.0f};
    float m_RoughnessFactor {1.0f};
    float m_NormalScale {1.0f};
    float m_OcclusionStrength {1.0f};
    Vector3 m_EmissiveFactor {0.0f, 0.0f, 0.0f};
    bool m_IsBlend {false};
    bool m_IsDoubleSided {false};

    /// 运行时属性块的 4 个 vec4 槽。见 MeshMaterialUniform::customParams 的注释：
    /// 内置 shader 在 keyword 打开时按约定消费（现在只有 `_DISSOLVE_ON` 那一组）。
    /// 脚本按名字写，名字到 (槽位, 分量) 的翻译在 MaterialCustomParamNames.h。
    Vector4 m_CustomParams[k_MaterialCustomParamCount] {};

    /// ShaderLab 具名属性：这个材质所用 shader 声明的属性在 UBO 里的落点。
    ///
    /// 内置 StandardLit 为空 —— 它的 UBO 是上面那 8 个标量 + 4 个 vec4 的固定布局，
    /// 没有"具名属性"这回事。只有自定义 ShaderLab 资产才有内容，由
    /// BaseRenderer::populateMaterialDesc 从 Material 指向的 ShaderRes 抄过来。
    ///
    /// 名字 -> offset 的翻译在这里就做完，SetFloat("_Whatever", v) 于是能立刻知道
    /// "这个名字认不认识"，不用等到上传 UBO 时才发现。
    std::vector<ZEngine::ShaderLab::ShaderPropertySlot> m_ShaderProperties;

    /// 资产侧保存的属性值（Material 的 Float/Color/Toggle 属性表 + shader 默认值），
    /// 已经按上面那张落点表铺进字节里。per-renderer 的 override 盖在它之上。
    ZEngine::ShaderLab::ShaderPropertyBlob m_PropertyBlob;

    // Resolved Texture2D .zasset paths at the render boundary (from Material PPtr refs).
    eastl::string m_BaseColorTextureFile;
    eastl::string m_MetallicRoughnessTextureFile;
    eastl::string m_NormalTextureFile;
    eastl::string m_OcclusionTextureFile;
    eastl::string m_EmissiveTextureFile;
    bool m_WithTexture {false};

    eastl::vector<eastl::string> m_EnabledShaderKeywords;

    /// 运行时材质实例开关。BaseRenderer 在某个 slot 上有 override 时置 true。
    ///
    /// false（默认）= 这个 part 用**材质资产的共享 GpuPBRMaterial**。上面那 8 个标量
    /// 虽然也是 per-part 带过来的，但共享材质的 MeshMaterialUniform 是第一次见到这个
    /// 资产时烤进去的，后面同资产的 part 改这些值不会上 GPU。
    ///
    /// true = RenderScene 给这个 part 一份**自己的 GpuPBRMaterial**（克隆共享材质：
    /// 复用纹理视图和采样器，自带 uniform buffer + 描述符集），于是上面那 8 个标量
    /// 就是这个 part 独有的。代价是这个 part 不再能和同资产的其它 part 合批。
    bool m_UseMaterialInstance {false};
};

class GameObjectTransformDesc
{
public:
    Matrix4x4 m_TransformMatrix {Matrix4x4::IDENTITY};
};

class GameObjectPartDesc
{
public:
    GameObjectMeshDesc m_MeshDesc;
    GameObjectMaterialDesc m_MaterialDesc;
    GameObjectTransformDesc m_TransformDesc;
    bool m_WithAnimation {false};
    SkeletonBindingDesc m_SkeletonBindingDesc;
    SkeletonAnimationResult m_SkeletonAnimationResult;
};

constexpr size_t k_invalid_part_id = std::numeric_limits<size_t>::max();

struct GameObjectPartId
{
    GObjectID m_GoId {k_invalid_gobject_id};
    size_t m_PartId {k_invalid_part_id};

    bool operator==(const GameObjectPartId& rhs) const { return m_GoId == rhs.m_GoId && m_PartId == rhs.m_PartId; }
    size_t getHashValue() const { return m_GoId ^ (m_PartId << 1); }
    bool IsValid() const { return m_GoId != k_invalid_gobject_id && m_PartId != k_invalid_part_id; }
};

class GameObjectDesc
{
public:
    GameObjectDesc()
        : m_GoId(0) {}
    GameObjectDesc(size_t go_id, const std::vector<GameObjectPartDesc>& parts)
        : m_GoId(go_id), m_ObjectParts(parts)
    {
    }

    GObjectID getId() const { return m_GoId; }
    const std::vector<GameObjectPartDesc>& getObjectParts() const { return m_ObjectParts; }

private:
    GObjectID m_GoId {k_invalid_gobject_id};
    std::vector<GameObjectPartDesc> m_ObjectParts;
};
template<>
struct std::hash<GameObjectPartId>
{
    size_t operator()(const GameObjectPartId& rhs) const noexcept { return rhs.getHashValue(); }
};
