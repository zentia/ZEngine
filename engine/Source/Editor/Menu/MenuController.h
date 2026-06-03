#pragma once

#include "Editor/EditorLayout/EditorLayoutConstants.h"
#include "Editor/Menu/ZSlateMenuBar.h"
#include "Editor/Platform/Interface/EditorView.h"
#include "Menu.h"
#include "Runtime/Slate/Application/SlateInput.h"

#include <memory>
#include <typeindex>

namespace ZSlate
{
class SWidget;
}

class MenuController : public EditorView
{
public:
    static constexpr float kMainMenuBarHeight = EditorLayoutConstants::kMainMenuBarHeight;
    static constexpr float kPlaybackToolbarHeight = EditorLayoutConstants::kPlaybackToolbarHeight;
    static constexpr float kReservedTopHeight = EditorLayoutConstants::kReservedTopHeight;

    explicit MenuController(EditorUI* editor_ui);
    ~MenuController();
    virtual void OnGUI() override;
    virtual EditorWindowState BeginGUI() override;

    // 通过类型获取菜单实例
    template<typename T>
    T* getMenu()
    {
        auto it = m_MenuMap.find(std::type_index(typeid(T)));
        if (it != m_MenuMap.end())
        {
            return static_cast<T*>(it->second);
        }
        return nullptr;
    }

private:
    void RegisterAllMenu();
    void UnregsiterAllMenu();
    // Native ZSlate playback toolbar: SButton + custom vector-icon leaf widgets
    // painted through the editor overlay, routed by a dedicated input router.
    // Rebuilt only when (playing / paused / scale) changes so click capture
    // survives a press->release across frames.
    void DrawPlaybackToolbarNative();
    void BuildPlaybackToolbar(bool playing, bool paused, float scale);

    std::shared_ptr<ZSlate::SWidget> m_PlaybackToolbar;
    ZSlate::SlateInputRouter m_PlaybackInput;
    bool m_BuiltPlaying {false};
    bool m_BuiltPaused {false};
    float m_BuiltToolbarScale {-1.0f};

    // Native ZSlate menu bar (r.ZSlate.NativeMenuBar). Builds the top-level entry
    // list lazily, paints + routes the bar into the editor overlay each frame, and
    // (P10b) registers a Foreground surface while a dropdown is open so native
    // panels under it report not-hovered (the native click-through guard that
    // replaced the old "##ZSlateMenuCapture" modal ImGui window in P10c).
    void RenderNativeMenuBar();
    void EnsureZSlateMenuEntries();

    ZSlate::ZSlateEditorMenuBar m_ZSlateMenuBar;
    bool m_ZSlateMenuEntriesBuilt {false};

    template<typename T>
    void registerMenu()
    {
        T* menu = MemoryManager::CreateObject<T>(this->m_EditorUi);
        m_Menus.insert(menu);
        m_MenuMap[std::type_index(typeid(T))] = menu;
    }
    std::set<Menu*> m_Menus;                               // 用于遍历所有菜单
    std::unordered_map<std::type_index, Menu*> m_MenuMap;  // 类型到实例的映射
};