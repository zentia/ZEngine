// Minimal stub for ZSlateInsightsWindow — excluded from build while ZSlate
// type bridging (UIRenderer->ISlateRenderer) is in progress.

#include "Editor/EditorWindow/ZSlateInsightsWindow/ZSlateInsightsWindow.h"
#include <string>

std::string ZSlateInsightsWindow::SaveTraceToDisk(const ZEngine::Insights::InsightsSnapshot&) { return {}; }
void ZSlateInsightsWindow::LaunchStandaloneViewer(const std::string&) {}
