#include "ZSlatePreviewWindow.h"

#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/EditorSceneManager/EditorSceneManager.h"
#include "Editor/EditorWindow/InspectorWindow/InspectorAssetCommon.h"    // ResolveInspectorAssetType / IsGenericInspectorZAssetType / IsTexture2DInspectorAssetType / LoadMaterialDefinitionForInspector
#include "Editor/EditorWindow/InspectorWindow/InspectorMaterialPreview.h"  // RenderMaterialPreviewToTexture
#include "Editor/EditorWindow/PreviewWindow/MeshDataPreview.h"           // IsSupportedAssetType / RenderToTexture
#include "Editor/EditorWindow/PreviewWindow/ShaderPreviewRenderer.h"     // RenderShaderPreviewToNativeTexture
#include "Editor/ZSlate/Backend/EditorSlateHost.h"
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"                   // native RHI backend

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Render/Texture/Texture2D.h"
#include "Runtime/Platform/Path/Path.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Slate/Application/SlateApplication.h"
#include "Runtime/Slate/Widgets/SBorder.h"
#include "Runtime/Slate/Widgets/SBox.h"
#include "Runtime/Slate/Widgets/SBoxPanel.h"
#include "Runtime/Slate/Widgets/SImage.h"
#include "Runtime/Slate/Widgets/SScrollBox.h"
#include "Runtime/Slate/Widgets/STextBlock.h"
#include "Runtime/UI/Render/UiGpuResources.h"  // EnsureTexture2D (native texture preview)

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace ZSlate;

namespace
{
    const UIColor kHeaderColor(0.92f, 0.93f, 0.97f, 1.0f);
    const UIColor kLabelColor(0.74f, 0.78f, 0.84f, 1.0f);
    const UIColor kValueColor(0.85f, 0.87f, 0.92f, 1.0f);
    const UIColor kDimColor(0.50f, 0.52f, 0.58f, 1.0f);

    // Largest edge (in unscaled px) the texture preview quad is allowed to occupy.
    constexpr float kPreviewMaxEdge = 256.0f;

    std::shared_ptr<STextBlock> MakeText(const char* text, float font_size, const UIColor& color)
    {
        auto t = std::make_shared<STextBlock>();
        t->Text = text;
        t->FontSize = font_size;
        t->Color = color;
        t->Alignment = TextAnchor::MiddleLeft;
        return t;
    }

    // Fixed-width label cell so value columns line up (mirrors the inspector's
    // MakeLabelBox).
    std::shared_ptr<SBox> MakeLabelBox(const std::string& text, float scale, float width)
    {
        auto box = std::make_shared<SBox>();
        box->WidthOverride = width;
        box->VAlign = EVerticalAlignment::Center;
        box->SetContent(MakeText(text.c_str(), 13.0f * scale, kLabelColor));
        return box;
    }

    // A "label : value" row (label left, value right) appended to a vertical box.
    void AddInfoRow(const std::shared_ptr<SVerticalBox>& column,
                    const std::string& label,
                    const std::string& value,
                    float scale)
    {
        auto row = std::make_shared<SHorizontalBox>();
        row->AddSlot(MakeLabelBox(label, scale, 72.0f * scale))
            .AutoSize()
            .SetVAlign(EVerticalAlignment::Center);
        row->AddSlot(MakeText(value.c_str(), 13.0f * scale, kValueColor))
            .Fill(1.0f)
            .SetVAlign(EVerticalAlignment::Center);

        column->AddSlot(row).AutoSize().SetPadding(FMargin(0.0f, 0.0f, 0.0f, 4.0f * scale));
    }

    // Common header: title + Name/Path rows + a thin separator. Returns the
    // column so the caller can append type-specific content below it.
    std::shared_ptr<SVerticalBox> MakeHeaderColumn(const std::filesystem::path& asset_path, float scale)
    {
        auto column = std::make_shared<SVerticalBox>();
        column->AddSlot(MakeText("Preview", 18.0f * scale, kHeaderColor))
            .AutoSize()
            .SetPadding(FMargin(0.0f, 0.0f, 0.0f, 8.0f * scale));

        const std::string asset_name =
            Path::GetFilePureName(asset_path.filename().generic_string().c_str()).c_str();
        AddInfoRow(column, "Name", asset_name, scale);
        AddInfoRow(column, "Path", asset_path.generic_string(), scale);

        // Thin separator line: a 1px-tall SBox (filled horizontally by the slot)
        // backed by an SBorder fill.
        auto sep_line = std::make_shared<SBorder>();
        sep_line->BackgroundColor = UIColor(0.25f, 0.26f, 0.30f, 1.0f);
        auto sep_box = std::make_shared<SBox>();
        sep_box->HeightOverride = 1.0f * scale;
        sep_box->SetContent(sep_line);
        column->AddSlot(sep_box).AutoSize().SetPadding(FMargin(0.0f, 4.0f * scale, 0.0f, 8.0f * scale));
        return column;
    }

    // Wrap a content column in a scrollable, padded panel and store it as the root.
    std::shared_ptr<SWidget> WrapPanel(const std::shared_ptr<SVerticalBox>& column, float scale)
    {
        auto scroll = std::make_shared<SScrollBox>();
        scroll->AddChild(column);
        auto border = std::make_shared<SBorder>();
        border->BackgroundColor = UIColor(0.10f, 0.10f, 0.13f, 1.0f);
        border->Padding = FMargin(16.0f * scale);
        border->HAlign = EHorizontalAlignment::Fill;
        border->VAlign = EVerticalAlignment::Fill;
        border->SetContent(scroll);
        return border;
    }
}  // namespace

ZSlatePreviewWindow::ZSlatePreviewWindow(EditorUI* editor_ui)
    : EditorWindow(editor_ui, EditorLayoutWindowIds::kPreview)
{
    m_Open = true;
    BuildEmpty(1.0f);
}

void ZSlatePreviewWindow::BuildEmpty(float scale)
{
    auto msg = MakeText("Select an asset to preview.", 16.0f * scale, kDimColor);
    msg->Alignment = TextAnchor::MiddleCenter;

    auto border = std::make_shared<SBorder>();
    border->BackgroundColor = UIColor(0.10f, 0.10f, 0.13f, 1.0f);
    border->Padding = FMargin(16.0f * scale);
    border->HAlign = EHorizontalAlignment::Fill;
    border->VAlign = EVerticalAlignment::Fill;
    border->SetContent(msg);
    m_Root = border;
}

void ZSlatePreviewWindow::BuildTexture(const std::filesystem::path& asset_path, float scale)
{
    auto column = MakeHeaderColumn(asset_path, scale);

    std::filesystem::path read_path = asset_path;
    Texture2D* texture = GET_SYSTEM(AssetManager)->ReadObject<Texture2D>(read_path);
    if (texture == nullptr || !texture->IsValid())
    {
        column->AddSlot(MakeText("Unable to load texture asset.", 14.0f * scale, kDimColor)).AutoSize();
        m_Root = WrapPanel(column, scale);
        return;
    }

    char dims[96];
    std::snprintf(dims, sizeof(dims), "%ux%u (format ord %u)", texture->m_Width, texture->m_Height,
                  texture->m_Format);
    AddInfoRow(column, "Imported", dims, scale);

    // Native path only: SImage.Texture is a UiGpuResources handle under the
    // native backend. Under the ImGui-bridge fallback it would be (mis)read as an
    // ImTextureID, so we show a message instead. Bridge parity is a follow-up.
    if (!ZSlate::ZSlateEditorOverlay::Get().IsNativeBackendEnabled())
    {
        column->AddSlot(MakeText("Texture preview requires the native ZSlate backend (r.ZSlate.NativeBackend=1).",
                                 13.0f * scale, kDimColor))
            .AutoSize()
            .SetPadding(FMargin(0.0f, 6.0f * scale, 0.0f, 0.0f));
        m_Root = WrapPanel(column, scale);
        return;
    }

    UiGpuResources* gpu = UiGpuResources::Get();
    void* handle = gpu != nullptr ? gpu->EnsureTexture2D(texture) : nullptr;
    if (handle == nullptr)
    {
        column->AddSlot(MakeText("Texture GPU upload unavailable.", 13.0f * scale, kDimColor)).AutoSize();
        m_Root = WrapPanel(column, scale);
        return;
    }

    // Aspect-fit the preview quad inside a kPreviewMaxEdge box.
    const float src_w = static_cast<float>(std::max<uint32_t>(1u, texture->m_Width));
    const float src_h = static_cast<float>(std::max<uint32_t>(1u, texture->m_Height));
    const float max_edge = kPreviewMaxEdge * scale;
    const float fit = std::min(max_edge / src_w, max_edge / src_h);
    const float draw_w = src_w * fit;
    const float draw_h = src_h * fit;

    auto image = std::make_shared<SImage>();
    image->Brush.Texture = handle;
    image->Brush.Tint = UIColor(1.0f, 1.0f, 1.0f, 1.0f);
    image->Brush.ImageSize = Vector2(draw_w, draw_h);
    column->AddSlot(image)
        .AutoSize()
        .SetHAlign(EHorizontalAlignment::Center)
        .SetPadding(FMargin(0.0f, 4.0f * scale, 0.0f, 0.0f));

    m_Root = WrapPanel(column, scale);
}

void ZSlatePreviewWindow::BuildMesh(const std::filesystem::path& asset_path, float scale)
{
    auto column = MakeHeaderColumn(asset_path, scale);

    // Native path only: SImage.Texture is a UiGpuResources handle under the
    // native backend (same constraint as the texture preview).
    if (!ZSlate::ZSlateEditorOverlay::Get().IsNativeBackendEnabled())
    {
        column->AddSlot(MakeText("Mesh preview requires the native ZSlate backend (r.ZSlate.NativeBackend=1).",
                                 13.0f * scale, kDimColor))
            .AutoSize()
            .SetPadding(FMargin(0.0f, 6.0f * scale, 0.0f, 0.0f));
        m_Root = WrapPanel(column, scale);
        return;
    }

    // Render at a fixed bitmap resolution scaled by the UI scale, displayed 1:1.
    const uint32_t pixel_size =
        static_cast<uint32_t>(std::clamp(static_cast<int>(256.0f * scale + 0.5f), 128, 512));
    const MeshDataPreview::PreviewFrame frame =
        MeshDataPreview::RenderToTexture(asset_path, pixel_size, MeshDataPreview::PreviewInput {});
    if (!frame.ok || frame.texture_handle == nullptr)
    {
        const std::string msg = frame.error.empty() ? "Mesh preview unavailable." : frame.error;
        column->AddSlot(MakeText(msg.c_str(), 14.0f * scale, kDimColor)).AutoSize();
        m_Root = WrapPanel(column, scale);
        return;
    }

    auto image = std::make_shared<SImage>();
    image->Brush.Texture = frame.texture_handle;
    image->Brush.Tint = UIColor(1.0f, 1.0f, 1.0f, 1.0f);
    image->Brush.ImageSize = Vector2(static_cast<float>(frame.pixel_size), static_cast<float>(frame.pixel_size));
    column->AddSlot(image)
        .AutoSize()
        .SetHAlign(EHorizontalAlignment::Center)
        .SetPadding(FMargin(0.0f, 4.0f * scale, 0.0f, 6.0f * scale));

    char stats[160];
    std::snprintf(stats, sizeof(stats), "%zu verts, %zu indices (%zu tris)", frame.vertex_count, frame.index_count,
                  frame.triangle_count);
    AddInfoRow(column, "Mesh", stats, scale);

    m_Root = WrapPanel(column, scale);
}

void ZSlatePreviewWindow::BuildMaterial(const std::filesystem::path& asset_path, float scale)
{
    auto column = MakeHeaderColumn(asset_path, scale);

    if (!ZSlate::ZSlateEditorOverlay::Get().IsNativeBackendEnabled())
    {
        column->AddSlot(MakeText("Material preview requires the native ZSlate backend (r.ZSlate.NativeBackend=1).",
                                 13.0f * scale, kDimColor))
            .AutoSize()
            .SetPadding(FMargin(0.0f, 6.0f * scale, 0.0f, 0.0f));
        m_Root = WrapPanel(column, scale);
        return;
    }

    Material material_from_disk;
    const Material* preview_material = nullptr;
    const Material* live_material = nullptr;
    if (TryGetInspectorLiveMaterialForPreview(asset_path, live_material))
    {
        preview_material = live_material;
    }
    else if (LoadMaterialDefinitionForInspector(material_from_disk, asset_path))
    {
        preview_material = &material_from_disk;
    }
    else
    {
        column->AddSlot(MakeText("Unable to load material asset.", 14.0f * scale, kDimColor)).AutoSize();
        m_Root = WrapPanel(column, scale);
        return;
    }

    const uint32_t pixel_size =
        static_cast<uint32_t>(std::clamp(static_cast<int>(256.0f * scale + 0.5f), 128, 512));
    const MaterialPreviewResult frame = RenderMaterialPreviewToTexture(*preview_material, pixel_size);
    if (!frame.ok || frame.texture_handle == nullptr)
    {
        const std::string msg = frame.error.empty() ? "Material preview unavailable." : frame.error;
        column->AddSlot(MakeText(msg.c_str(), 14.0f * scale, kDimColor)).AutoSize();
        m_Root = WrapPanel(column, scale);
        return;
    }

    auto image = std::make_shared<SImage>();
    image->Brush.Texture = frame.texture_handle;
    image->Brush.Tint = UIColor(1.0f, 1.0f, 1.0f, 1.0f);
    image->Brush.ImageSize = Vector2(static_cast<float>(frame.pixel_size), static_cast<float>(frame.pixel_size));
    column->AddSlot(image)
        .AutoSize()
        .SetHAlign(EHorizontalAlignment::Center)
        .SetPadding(FMargin(0.0f, 4.0f * scale, 0.0f, 6.0f * scale));

    if (!frame.texture_summary.empty())
    {
        column->AddSlot(MakeText(frame.texture_summary.c_str(), 12.0f * scale, kDimColor)).AutoSize();
    }

    m_Root = WrapPanel(column, scale);
}

void ZSlatePreviewWindow::BuildShader(const std::filesystem::path& asset_path,
                                     float scale,
                                     void* texture_handle,
                                     const std::string& message)
{
    auto column = MakeHeaderColumn(asset_path, scale);

    if (!ZSlate::ZSlateEditorOverlay::Get().IsNativeBackendEnabled())
    {
        column->AddSlot(MakeText("Shader preview requires the native ZSlate backend (r.ZSlate.NativeBackend=1).",
                                 13.0f * scale, kDimColor))
            .AutoSize()
            .SetPadding(FMargin(0.0f, 6.0f * scale, 0.0f, 0.0f));
        m_Root = WrapPanel(column, scale);
        return;
    }

    if (texture_handle == nullptr)
    {
        // The RTT is rendered synchronously on selection (RunSynchronizedGpuReadback
        // + a dedicated command list), so a null handle here means the GPU render
        // failed (shader compile error / unsupported bindings) -- surface the
        // message. OnGUI rebuilds the moment the handle becomes valid.
        const std::string hint = message.empty() ? "Preparing shader preview..." : message;
        column->AddSlot(MakeText(hint.c_str(), 13.0f * scale, kDimColor))
            .AutoSize()
            .SetPadding(FMargin(0.0f, 6.0f * scale, 0.0f, 0.0f));
        m_Root = WrapPanel(column, scale);
        return;
    }

    // RTT square; SImage scales it to the requested display edge.
    const float edge = std::clamp(256.0f * scale, 160.0f, 420.0f);
    auto image = std::make_shared<SImage>();
    image->Brush.Texture = texture_handle;
    image->Brush.Tint = UIColor(1.0f, 1.0f, 1.0f, 1.0f);
    image->Brush.ImageSize = Vector2(edge, edge);
    column->AddSlot(image)
        .AutoSize()
        .SetHAlign(EHorizontalAlignment::Center)
        .SetPadding(FMargin(0.0f, 4.0f * scale, 0.0f, 6.0f * scale));

    column->AddSlot(MakeText("DX12 offscreen shader execution", 12.0f * scale, kDimColor)).AutoSize();

    m_Root = WrapPanel(column, scale);
}

void ZSlatePreviewWindow::BuildPlaceholder(PreviewKind kind, const std::filesystem::path& asset_path, float scale)
{
    (void)kind;
    auto column = MakeHeaderColumn(asset_path, scale);

    column->AddSlot(MakeText("No preview available for this asset type.", 14.0f * scale, kDimColor)).AutoSize();
    m_Root = WrapPanel(column, scale);
}

void ZSlatePreviewWindow::OnGUI()
{
    float ui_scale = ZSlate::EditorSlateHost::Get().GetUiScale();
    if (ui_scale < 0.5f)
        ui_scale = 1.0f;

    // Classify the current selection.
    PreviewKind kind = PreviewKind::None;
    std::filesystem::path asset_path = GET_SYSTEM(EditorSceneManager)->getSelectedAssetPath();
    if (!asset_path.empty())
    {
        const std::string selected_type = GET_SYSTEM(EditorSceneManager)->getSelectedAssetType();
        const std::string resolved = ResolveInspectorAssetType(asset_path, selected_type);
        if (IsGenericInspectorZAssetType(asset_path, resolved))
            kind = PreviewKind::Unsupported;
        else if (IsTexture2DInspectorAssetType(resolved))
            kind = PreviewKind::Texture;
        else if (MeshDataPreview::IsSupportedAssetType(resolved))
            kind = PreviewKind::Mesh;
        else if (resolved == "material")
            kind = PreviewKind::Material;
        else if (resolved == "shader")
            kind = PreviewKind::Shader;
        else
            kind = PreviewKind::Unsupported;
    }

    auto& overlay = ZSlate::ZSlateEditorOverlay::Get();

    // Drive the DX12 RTT shader pass while a shader is selected. The renderer
    // gates internally: the offscreen RTT is redrawn only when the selection /
    // shader source changes (the GPU work runs synchronously via
    // RunSynchronizedGpuReadback). The returned handle is stable across frames;
    // the tree is rebuilt when it first appears / changes.
    void* shader_handle = nullptr;
    std::string shader_message;
    if (kind == PreviewKind::Shader)
    {
        const ShaderPreviewTextureResult shader_result =
            RenderShaderPreviewToNativeTexture(asset_path, "shader", nullptr);
        shader_handle = shader_result.texture_handle;
        shader_message = shader_result.message;
    }

    const std::string asset_key = (kind == PreviewKind::None) ? std::string() : asset_path.generic_string();
    const uint64_t material_preview_revision =
        (kind == PreviewKind::Material) ? GetInspectorLiveMaterialPreviewRevision() : 0;
    const bool rebuild = m_ForceRebuild || (m_Root == nullptr) || (ui_scale != m_BuiltScale) ||
                         (kind != m_BuiltKind) || (asset_key != m_BuiltAssetPath) ||
                         (kind == PreviewKind::Shader && shader_handle != m_BuiltShaderHandle) ||
                         (kind == PreviewKind::Material &&
                          material_preview_revision != m_BuiltMaterialPreviewRevision);
    if (rebuild)
    {
        m_ForceRebuild = false;
        switch (kind)
        {
            case PreviewKind::None:
                BuildEmpty(ui_scale);
                break;
            case PreviewKind::Texture:
                BuildTexture(asset_path, ui_scale);
                break;
            case PreviewKind::Mesh:
                BuildMesh(asset_path, ui_scale);
                break;
            case PreviewKind::Material:
                BuildMaterial(asset_path, ui_scale);
                break;
            case PreviewKind::Shader:
                BuildShader(asset_path, ui_scale, shader_handle, shader_message);
                break;
            default:
                BuildPlaceholder(kind, asset_path, ui_scale);
                break;
        }
        m_BuiltScale = ui_scale;
        m_BuiltKind = kind;
        m_BuiltAssetPath = asset_key;
        m_BuiltShaderHandle = shader_handle;
        m_BuiltMaterialPreviewRevision = material_preview_revision;
        m_Input.Reset();
    }

    float pos_x = 0.0f;
    float pos_y = 0.0f;
    float avail_w = 0.0f;
    float avail_h = 0.0f;
    // Native dock hosting is unconditional: ReconcileNativeTreeWithOpenWindows (run
    // before any panel OnGUI) guarantees an open window is in the dock tree, so the
    // leaf rect always comes from NativeRect().
    const float* native_rect = NativeRect();
    pos_x = native_rect[0];
    pos_y = native_rect[1];
    avail_w = native_rect[2];
    avail_h = native_rect[3];
    if (avail_w < 1.0f)
        avail_w = 1.0f;
    if (avail_h < 1.0f)
        avail_h = 1.0f;

    const UIRect region(pos_x, pos_y, avail_w, avail_h);
    const FGeometry geometry(Vector2(pos_x, pos_y), Vector2(avail_w, avail_h));

    {
        BatchedUIRenderer& renderer = overlay.GetRenderer();
        overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZPanel);

        m_Root->CacheDesiredSize();

        FPaintContext ctx;
        ctx.Renderer = &renderer;
        ctx.LayerId = 0;

        renderer.pushClipRect(region, true);
        m_Root->Paint(ctx, geometry);
        renderer.popClipRect();
    }

    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const int surface_id = ZSlate::EditorSlateHost::HashId(m_Title);
    host.BeginSurface(surface_id, region, ZSlate::ESurfaceLayer::Panels);
    const Vector2 mouse = host.GetPointerPos();
    const bool over_canvas = host.IsSurfaceHovered(surface_id, mouse);
    const bool left_down = host.IsLeftDown();
    const float wheel = over_canvas ? host.GetWheelDelta() : 0.0f;

    m_Input.ProcessMouse(m_Root, mouse, over_canvas, left_down, wheel);
}
