// =============================================================================
// ASTCPreviewWindow.cpp - TEMPORARILY DISABLED
// -----------------------------------------------------------------------------
// Implementation of ASTC texture preview window.
// 
// TODO: Enable this after ZEditor compiles successfully.
// =============================================================================

#include "ASTCPreviewWindow.h"

// Logging
#include "Runtime/Core/Base/Macro.h"

ASTCPreviewWindow::ASTCPreviewWindow(EditorUI* editor_ui)
    : EditorWindow(editor_ui, "ASTC Texture Preview")
{
    // m_Title is set by EditorWindow constructor
    LOG_INFO(ZEditor, "ASTCPreviewWindow: Constructor called (DISABLED)");
}

void ASTCPreviewWindow::SetTexture(const std::filesystem::path& texture_path)
{
    LOG_WARNING(ZEditor, "ASTCPreviewWindow: SetTexture called but feature is DISABLED");
}

void ASTCPreviewWindow::OnGUI()
{
    LOG_INFO(ZEditor, "ASTCPreviewWindow: OnGUI called (DISABLED)");
}

void ASTCPreviewWindow::BuildPreviewUI(float scale)
{
    LOG_INFO(ZEditor, "ASTCPreviewWindow: BuildPreviewUI called (DISABLED)");
}

bool ASTCPreviewWindow::DecompressTexture()
{
    LOG_WARNING(ZEditor, "ASTCPreviewWindow: DecompressTexture called but feature is DISABLED");
    return false;
}
