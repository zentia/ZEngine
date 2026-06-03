#pragma once

#include "Editor/EditorWindow/EditorWindow.h"
#include "Runtime/Slate/Application/SlateInput.h"
#include "Runtime/Slate/Core/SlatePaint.h"
#include "Runtime/UI/Render/UIRenderer.h"

#include <filesystem>
#include <memory>
#include <string>

namespace ZSlate
{
class SWidget;
}

// Native ZSlate replacement for the legacy ImGui PreviewWindow. Shows the
// selected asset's name/path header plus a type-specific preview, all displayed
// through SImage under the native backend:
//   - Texture2D  -> UiGpuResources::EnsureTexture2D
//   - Mesh       -> software rasterizer -> UiGpuResources::UpdateDynamicTexture
//   - Material   -> software rasterizer -> UiGpuResources::UpdateDynamicTexture
//   - Shader     -> DX12 render-to-texture adopted via
//                   UiGpuResources::AdoptExternalImageView (real offscreen shader
//                   execution). The RTT GPU pass is driven per-frame from OnGUI;
//                   the tree is rebuilt once the handle first becomes valid.
class ZSlatePreviewWindow : public EditorWindow
{
public:
    explicit ZSlatePreviewWindow(EditorUI* editor_ui);
    void OnGUI() override;
    bool SupportsNativeHosting() const override { return true; }

private:
    enum class PreviewKind
    {
        None,         // nothing selected
        Texture,      // Texture2D zasset
        Mesh,         // MeshData zasset
        Material,     // material zasset
        Shader,       // shader zasset
        Unsupported,  // selected but not previewable
    };

    void BuildEmpty(float scale);
    void BuildTexture(const std::filesystem::path& asset_path, float scale);
    void BuildMesh(const std::filesystem::path& asset_path, float scale);
    void BuildMaterial(const std::filesystem::path& asset_path, float scale);
    void BuildShader(const std::filesystem::path& asset_path, float scale, void* texture_handle, const std::string& message);
    void BuildPlaceholder(PreviewKind kind, const std::filesystem::path& asset_path, float scale);

    std::shared_ptr<ZSlate::SWidget> m_Root;

    ZSlate::SlateInputRouter m_Input;

    float m_BuiltScale {-1.0f};
    PreviewKind m_BuiltKind {PreviewKind::None};
    std::string m_BuiltAssetPath;
    void* m_BuiltShaderHandle {nullptr};
    uint64_t m_BuiltMaterialPreviewRevision {0};
    bool m_ForceRebuild {false};
};
