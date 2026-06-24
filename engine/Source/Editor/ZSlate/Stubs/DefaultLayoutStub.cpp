// Minimal stub for DefaultLayout — excluded from build while ZSlate
// type bridging (UIRenderer->ISlateRenderer) is in progress.

#include "Editor/EditorLayout/DefaultLayout/DefaultLayout.h"
#include <string>
#include <vector>

DefaultLayout::DefaultLayout(EditorUI* editor_ui) : EditorLayout(editor_ui) {}
DefaultLayout::~DefaultLayout() = default;
void DefaultLayout::OnGUI() {}
void DefaultLayout::DrawDialogs() {}
void DefaultLayout::QueueBuiltinLayout(const std::string&) {}
void DefaultLayout::OpenSaveLayoutDialog() {}
void DefaultLayout::QueueUserLayout(const std::string&) {}
void DefaultLayout::SaveCurrentLayoutToFileDialog() {}
void DefaultLayout::LoadLayoutFromFileDialog() {}
void DefaultLayout::DeleteLayout(const std::string&) {}
void DefaultLayout::ResetAllLayouts() {}
std::vector<std::string> DefaultLayout::GetBuiltinLayoutNames() const { return {}; }
std::vector<std::string> DefaultLayout::GetUserLayoutNames() const { return {}; }
bool DefaultLayout::IsCurrentLayout(const std::string&) const { return false; }
bool DefaultLayout::QueryNativeDockPanel(const char*, float*, bool&) const { return false; }
