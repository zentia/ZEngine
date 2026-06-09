// =============================================================================
// TexPreviewWindow.cpp - TEMPORARILY DISABLED
// -----------------------------------------------------------------------------
// Implementation of block-compressed texture preview window.
// 
// TODO: Enable this after ZEditor compiles successfully.
// =============================================================================

#include "TexPreviewWindow.h"

// Logging
#include "Runtime/Core/Base/Macro.h"

TexPreviewWindow::TexPreviewWindow(EditorUI* editor_ui)
    : EditorWindow(editor_ui, "Texture Preview")
{
    LOG_INFO(ZEditor, "TexPreviewWindow: Constructor called (DISABLED)");
}

void TexPreviewWindow::SetTexture(const std::filesystem::path& texture_path)
{
    LOG_WARNING(ZEditor, "TexPreviewWindow: SetTexture called but feature is DISABLED");
}

void TexPreviewWindow::OnGUI()
{
    LOG_INFO(ZEditor, "TexPreviewWindow: OnGUI called (DISABLED)");
}

void TexPreviewWindow::BuildPreviewUI(float scale)
{
    LOG_INFO(ZEditor, "TexPreviewWindow: BuildPreviewUI called (DISABLED)");
}

bool TexPreviewWindow::DecompressTexture()
{
    LOG_WARNING(ZEditor, "TexPreviewWindow: DecompressTexture called but feature is DISABLED");
    return false;
}
