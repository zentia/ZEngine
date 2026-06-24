// Minimal stub for FloatingPanelManager — excluded from build while ZSlate
// type bridging (UIRenderer->ISlateRenderer) is in progress.

#include "Editor/FloatingPanel/FloatingPanelManager.h"

// FloatingPanel is defined in FloatingPanelManager.cpp (hidden impl).
// Provide a minimal definition so the unique_ptr destructor can compile.
struct FloatingPanelManager::FloatingPanel {
    ~FloatingPanel() = default;
};

FloatingPanelManager& FloatingPanelManager::Get()
{
    static FloatingPanelManager s_instance;
    return s_instance;
}

FloatingPanelManager::FloatingPanelManager() = default;
FloatingPanelManager::~FloatingPanelManager() = default;
void FloatingPanelManager::Initialize(EditorUI*) {}
void FloatingPanelManager::Shutdown() {}
void FloatingPanelManager::TickMainThread() {}
void FloatingPanelManager::BuildBatches() {}
void FloatingPanelManager::DrawSurfaces(RHI*) {}
bool FloatingPanelManager::HasFloatingPanels() const { return false; }
void FloatingPanelManager::RequestFloat(const std::string&) {}
void FloatingPanelManager::RequestDock(const std::string&) {}
bool FloatingPanelManager::IsFloating(const std::string&) const { return false; }
