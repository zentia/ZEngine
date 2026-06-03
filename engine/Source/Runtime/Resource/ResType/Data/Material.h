#pragma once
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Math/Vector3.h"

#include <vector>

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
class Material : public Object
{
    REGISTER_CLASS(Material)
    DECLARE_OBJECT_SERIALIZE(Material)

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
