#pragma once
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Math/Vector3.h"

#include <vector>
#include "Material.generated.h"

class ShaderRes;
class Texture2D;

// Unity MaterialPropertyBlock-style saved property entries (shader-driven extras).
struct MaterialFloatProperty
{
    eastl::string m_Name;
    float m_Value {0.0f};

    static const char* GetTypeString() { return "MaterialFloatProperty"; }
    static bool AllowTransferOptimization() { return false; }

    template<typename TransferFunction>
    void Transfer(TransferFunction& transfer)
    {
        transfer.Transfer(m_Name, "name");
        transfer.Transfer(m_Value, "value");
    }
};

struct MaterialColorProperty
{
    eastl::string m_Name;
    Vector3 m_Color {1.0f, 1.0f, 1.0f};
    float m_Alpha {1.0f};

    static const char* GetTypeString() { return "MaterialColorProperty"; }
    static bool AllowTransferOptimization() { return false; }

    template<typename TransferFunction>
    void Transfer(TransferFunction& transfer)
    {
        transfer.Transfer(m_Name, "name");
        transfer.Transfer(m_Color, "color");
        transfer.Transfer(m_Alpha, "alpha");
    }
};

struct MaterialTextureProperty
{
    eastl::string m_Name;
    PPtr<Texture2D> m_Texture;

    static const char* GetTypeString() { return "MaterialTextureProperty"; }
    static bool AllowTransferOptimization() { return false; }

    template<typename TransferFunction>
    void Transfer(TransferFunction& transfer);
};

struct MaterialToggleProperty
{
    eastl::string m_Name;
    bool m_Value {false};

    static const char* GetTypeString() { return "MaterialToggleProperty"; }
    static bool AllowTransferOptimization() { return false; }

    template<typename TransferFunction>
    void Transfer(TransferFunction& transfer)
    {
        transfer.Transfer(m_Name, "name");
        transfer.Transfer(m_Value, "value");
    }
};

// Unity-style material asset: shader + saved properties + built-in PBR slots.
// Texture and shader bindings are asset references (PPtr), not source file paths.
//
// 为什么一个方法都没标 ZFUNCTION（2026-10-26 核过一遍，别再重做）：
//
//  1. **脚本拿不到 Material 实例**。全仓库没有任何返回 `Material*` / `PPtr<Material>`
//     的接口；`BaseRenderer` 只有 `m_SharedMaterialAssets`（一串 .zasset 路径），
//     材质在 PostLoadResource 时按路径解析成渲染层的 `GpuPBRMaterial`。把一个脚本
//     无从获得的类型的方法全导出去 = 一堆死 API。
//  2. **没有 Unity 那种"每个 renderer 一份可运行时改的材质实例"**。想让脚本做
//     受击闪白 / 溶解 / 改描边颜色，得先有"材质实例化 + 属性块上传"这条链路，那是
//     功能开发，不是补绑定。
//  3. `MaterialFloatProperty` / `MaterialColorProperty` / `MaterialTextureProperty` /
//     `MaterialToggleProperty` 这四个 vector 是 Unity MaterialPropertyBlock 式的
//     **资产侧**保存项，没有运行时 upsert 入口 —— 就算导出去也没有 SetFloat 可配。
//
// 结论：材质要进脚本，先做"运行时材质实例"，再谈绑定。
ZCLASS(CustomTransfer)
class Material : public Object
{
    ZGENERATED_BODY(Material)

public:
    // Built-in / legacy shader name string. Used when m_ShaderPptr is null
    // (engine shaders such as "StandardLit"). Project shaders resolve via PPtr.
    eastl::string m_Shader {"StandardLit"};
    eastl::string m_ShaderGuid;
    PPtr<ShaderRes> m_ShaderPptr;

    std::vector<MaterialFloatProperty> m_FloatProperties;
    std::vector<MaterialColorProperty> m_ColorProperties;
    std::vector<MaterialTextureProperty> m_TextureProperties;
    std::vector<MaterialToggleProperty> m_ToggleProperties;

    Vector3 m_BaseColorFactor {1.0f, 1.0f, 1.0f};
    float m_AlphaFactor {1.0f};
    float m_MetallicFactor {1.0f};
    float m_RoughnessFactor {1.0f};
    float m_NormalScale {1.0f};
    float m_OcclusionStrength {1.0f};
    Vector3 m_EmissiveFactor {0.0f, 0.0f, 0.0f};
    bool m_IsBlend {false};
    bool m_IsDoubleSided {false};

    PPtr<Texture2D> m_BaseColourTexturePptr;
    PPtr<Texture2D> m_MetallicRoughnessTexturePptr;
    PPtr<Texture2D> m_NormalTexturePptr;
    PPtr<Texture2D> m_OcclusionTexturePptr;
    PPtr<Texture2D> m_EmissiveTexturePptr;

    std::vector<eastl::string> m_EnabledShaderKeywords;

    eastl::string GetShaderName() const;
    void SetShaderByName(const eastl::string& name);

    // Resolve a Texture2D PPtr to its on-disk .zasset path (project-relative when
    // possible). Empty when unassigned.
    static eastl::string ResolveTextureAssetPath(const PPtr<Texture2D>& texture);
    static bool AssignTextureFromAssetPath(PPtr<Texture2D>& out_texture, const eastl::string& asset_path);

    eastl::string GetBaseColourTextureFile() const;
    eastl::string GetMetallicRoughnessTextureFile() const;
    eastl::string GetNormalTextureFile() const;
    eastl::string GetOcclusionTextureFile() const;
    eastl::string GetEmissiveTextureFile() const;

    bool IsShaderKeywordEnabled(const eastl::string& keyword) const;
    void SetShaderKeywordEnabled(const eastl::string& keyword, bool enabled);
    void ClearShaderKeywords();
    eastl::string BuildShaderVariantKeyString() const;
};

// Legacy type name for on-disk .zasset headers written before the Material rename.
using MaterialRes = Material;
