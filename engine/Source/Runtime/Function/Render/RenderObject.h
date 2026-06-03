#pragma once

#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"

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

    // Resolved Texture2D .zasset paths at the render boundary (from Material PPtr refs).
    eastl::string m_BaseColorTextureFile;
    eastl::string m_MetallicRoughnessTextureFile;
    eastl::string m_NormalTextureFile;
    eastl::string m_OcclusionTextureFile;
    eastl::string m_EmissiveTextureFile;
    bool m_WithTexture {false};

    eastl::vector<eastl::string> m_EnabledShaderKeywords;
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
