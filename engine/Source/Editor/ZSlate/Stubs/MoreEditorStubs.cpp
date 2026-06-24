// Minimal stubs for excluded ZSlate editor windows — linked by other editor
// modules while the UIRenderer->ISlateRenderer type migration is in progress.

#include "Editor/EditorWindow/ZSlateConsoleWindow/ZSlateConsoleWindow.h"
#include "Editor/EditorWindow/ZSlateContentBrowserWindow/ZSlateContentBrowserWindow.h"
#include "Editor/EditorWindow/ZSlateGameWindow/ZSlateGameWindow.h"

// ---- ZSlateConsoleWindow ----
void ZSlateConsoleWindow::PumpBufferedLogsIfOpen() {}

// ---- ZSlateContentBrowserWindow ----
void ZSlateContentBrowserWindow::ExecutePendingImportDialog() {}

// ---- ZSlateGameWindow ----
ZSlateGameWindow::ZSlateGameWindow(EditorUI* editor_ui)
    : PlayModeView(editor_ui, "Game", static_cast<ViewportType>(0)) {}
void ZSlateGameWindow::OnGUI() {}
