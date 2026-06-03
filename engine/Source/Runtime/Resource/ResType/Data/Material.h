#pragma once
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Math/Vector3.h"

#include <vector>

class ShaderRes;

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
    eastl::string m_TextureFile;

    static const char* GetTypeString() { return "MaterialTextureProperty"; }
    static bool AllowTransferOptimization() { return false; }

    template<typename TransferFunction>
    void Transfer(TransferFunction& transfer)
    {
        transfer.Transfer(m_Name, "name");
        transfer.Transfer(m_TextureFile, "texture_file");
    }
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

class MaterialRes : public Object
{
    REGISTER_CLASS(MaterialRes)
    DECLARE_OBJECT_SERIALIZE(MaterialRes)

public:
    // PR-SE3a-migrate (shadow phase):
    //   m_Shader      — legacy by-name reference. KEPT for read-backward-
    //                   compat: old .zasset files carry this field, and
    //                   SafeBinaryRead populates it when the on-disk
    //                   TypeTree has "shader". Also serves as the cheap
    //                   string accessor so downstream code doesn't need to
    //                   dereference PPtr on every frame. Always kept in
    //                   sync with m_ShaderPptr by SetShaderByName().
    //   m_ShaderGuid — legacy GUID string. Kept for the same read-
    //                   backward-compat reason; new writes leave it empty.
    //   m_ShaderPptr — primary reference (PR-SE3a-migrate). When valid,
    //                   it points at the ShaderRes .zasset in AssetRegistry.
    //                   When null (old .zasset or built-in "StandardLit"),
    //                   GetShaderName() falls back to m_Shader.
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

    eastl::string m_BaseColourTextureFile;
    eastl::string m_MetallicRoughnessTextureFile;
    eastl::string m_NormalTextureFile;
    eastl::string m_OcclusionTextureFile;
    eastl::string m_EmissiveTextureFile;

    /// Enabled `#pragma multi_compile` / `shader_feature` keywords for this
    /// material instance (Inspector toggles). Empty = default variant (no defines).
    std::vector<eastl::string> m_EnabledShaderKeywords;

    // PR-SE3a-migrate accessors
    eastl::string GetShaderName() const;
    void SetShaderByName(const eastl::string& name);

    bool IsShaderKeywordEnabled(const eastl::string& keyword) const;
    void SetShaderKeywordEnabled(const eastl::string& keyword, bool enabled);
    void ClearShaderKeywords();

    /// Canonical `k=1;` string for pipeline-cache keys (sorted keywords).
    eastl::string BuildShaderVariantKeyString() const;
};
