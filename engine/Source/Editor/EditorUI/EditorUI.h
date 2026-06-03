#pragma once

#include "Editor/Axis/Axis.h"
#include "Editor/EditorFileService/EditorFileService.h"
#include "Editor/EditorUI/EditorWindowRegistry.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Function/Render/RenderPassBase.h"
#include "Runtime/UI/Core/WindowUI.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

class EditorWindow;
class MenuController;
class DefaultLayout;

namespace ZSlate
{
class SMenu;
}

class EditorUI : public WindowUI
{
public:
    EditorUI();
    ~EditorUI();

    void ShowEditorUI();
    // Appends per-category submenus of window toggles into `parent`
    // (native ZSlate menu-bar path).
    void BuildEditorWindowZSlateMenu(ZSlate::SMenu& parent, bool exclude_package_manager, float scale);
    void ShowEditorWindowDock();
    void RegisterAllEditorWindow();
    void UnregisterAllEditorWindow();
    void RegisterEditorWindow(const EditorWindowDescriptor& descriptor);

    template<typename T>
    T* GetWindow()
    {
        auto it = m_WindowMap.find(std::type_index(typeid(T)));
        if (it != m_WindowMap.end())
        {
            return static_cast<T*>(it->second);
        }
        return nullptr;
    }

    template<typename T>
    const char* getWindowName()
    {
        return GetWindow<T>()->m_Title;
    }

    std::vector<EditorWindow*> GetEditorWindows() const;
    std::unordered_map<std::string, bool> GetEditorWindowOpenStates() const;
    void ApplyEditorWindowOpenStates(
        const std::unordered_map<std::string, bool>& states);
    EditorWindow* FindEditorWindow(const std::string& title) const;
    DefaultLayout* getLayoutManager() const { return m_EditorLayout; }
    void UnregisterEditorWindow(EditorWindow* editor_window);

    // Build ImGui on the game thread (GLFW input); GPU submit stays on the RHI worker.
    void PrepareGameThreadImGuiFrame();
    // Must run on the game thread before the next NewFrame() when parallel rendering is active.
    void FinalizePendingImGuiPlatformFrame();

    virtual bool Initialize() override final;
    virtual void PreRender() override final;
    // Main-thread deferred UI work (file dialogs, blocking import, ...).
    void ProcessDeferredWork();

private:
    std::set<EditorWindow*> m_EditorWindows;
    std::unordered_map<std::type_index, EditorWindow*> m_WindowMap;
    std::shared_ptr<MenuController> m_MenuController;
    DefaultLayout* m_EditorLayout;

    // Editor-owned UI render pass (native ZSlate overlay; ImGui-free).
    std::shared_ptr<RenderPassBase> m_EditorUiPass;
    bool m_ImGuiAwaitingPlatformUpdate {false};
};
