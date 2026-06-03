#include "EditorUI.h"

#include "Editor/EditorInputManager/EditorInputManager.h"
#include "Editor/EditorLayout/DefaultLayout/DefaultLayout.h"
#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/EditorSceneManager/EditorSceneManager.h"
#include "Editor/EditorScenePlacement/EditorScenePlacement.h"
#include "Editor/FloatingPanel/FloatingPanelManager.h"
#include "Editor/EditorUI/EditorWindowRegistry.h"
#include "Editor/EditorWindow/EditorWindow.h"
#include "Editor/EditorWindow/ZSlateProjectWindow/ZSlateProjectWindow.h"
#include "Editor/Menu/MenuController.h"
#include "Editor/Platform/Interface/GUIView.h"
#include "Editor/Render/Pass/EditorUIPass.h"
#include "Runtime/Core/Base/Factory.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Framework/Component/Mesh/MeshRenderer.h"
#include "Runtime/Function/Framework/Component/Transform/TransformComponent.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Input/InputSystem.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderPass.h"
#include "Runtime/Function/Render/RenderPipelineBase.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/RenderingThread/RenderingThread.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/Platform/Path/Path.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Slate/Widgets/SMenu.h"
#include "Runtime/Resource/Config/ConfigManager.h"

#include <stb_image.h>

#include <algorithm>
#include <filesystem>

namespace
{
    EditorUIPass* AsEditorUIPass(RenderPassBase* pass)
    {
        return static_cast<EditorUIPass*>(pass);
    }

    void LoadGlfwWindowIcons(GLFWwindow* window, const std::string& big_icon_path, const std::string& small_icon_path)
    {
        GLFWimage window_icon[2] = {};
        window_icon[0].pixels =
            stbi_load(big_icon_path.c_str(), &window_icon[0].width, &window_icon[0].height, nullptr, 4);
        window_icon[1].pixels =
            stbi_load(small_icon_path.c_str(), &window_icon[1].width, &window_icon[1].height, nullptr, 4);

        if (window_icon[0].pixels != nullptr && window_icon[1].pixels != nullptr)
        {
            glfwSetWindowIcon(window, 2, window_icon);
        }
        else
        {
            LOG_WARNING(ZEditor,
                        "EditorUI: failed to load window icons (big='{}' small='{}'); continuing without custom icon",
                        big_icon_path,
                        small_icon_path);
        }

        if (window_icon[0].pixels != nullptr)
        {
            stbi_image_free(window_icon[0].pixels);
        }
        if (window_icon[1].pixels != nullptr)
        {
            stbi_image_free(window_icon[1].pixels);
        }
    }
}  // namespace

EditorUI::EditorUI()
{
    m_MenuController = std::make_shared<MenuController>(this);
    m_EditorLayout = MemoryManager::CreateObject<DefaultLayout>(this);
}

EditorUI::~EditorUI()
{
    FloatingPanelManager::Get().Shutdown();
    UnregisterAllEditorWindow();
    if (m_EditorLayout != nullptr)
    {
        MemoryManager::DestroyObject(m_EditorLayout);
        m_EditorLayout = nullptr;
    }
}

void EditorUI::ShowEditorUI()
{
    m_MenuController->BeginGUI();
    m_MenuController->OnGUI();
    m_MenuController->EndGUI();

    if (m_EditorLayout != nullptr)
    {
        m_EditorLayout->DrawDialogs();
    }

    for (const auto& editor_window : m_EditorWindows)
    {
        auto state = editor_window->BeginGUI();
        if (state == EditorWindowState::Closed)
            continue;
        if (state == EditorWindowState::Opened)
            editor_window->OnGUI();
        editor_window->EndGUI();
    }
    GUIView::RepaintAll(true);
}

void EditorUI::BuildEditorWindowZSlateMenu(ZSlate::SMenu& parent, bool exclude_package_manager, float scale)
{
    auto category_for_title = [](const char* title) -> EditorWindowCategory {
        for (const EditorWindowDescriptor& descriptor : EditorWindowRegistry::GetBuiltinDescriptors())
        {
            if (title != nullptr && descriptor.dock_title != nullptr && std::string(title) == descriptor.dock_title)
            {
                return descriptor.category;
            }
        }
        return EditorWindowCategory::General;
    };

    static constexpr EditorWindowCategory kCategoryOrder[] = {EditorWindowCategory::General,
                                                              EditorWindowCategory::Scene,
                                                              EditorWindowCategory::Animation,
                                                              EditorWindowCategory::Package};

    auto category_label = [](EditorWindowCategory category) -> const char* {
        switch (category)
        {
            case EditorWindowCategory::Scene: return "Scene";
            case EditorWindowCategory::Animation: return "Animation";
            case EditorWindowCategory::Package: return "Package";
            case EditorWindowCategory::General:
            default: return "General";
        }
    };

    auto editor_windows = GetEditorWindows();
    std::sort(editor_windows.begin(),
              editor_windows.end(),
              [](const EditorWindow* lhs, const EditorWindow* rhs) { return std::string(lhs->m_Title) < std::string(rhs->m_Title); });

    for (const EditorWindowCategory category : kCategoryOrder)
    {
        std::vector<EditorWindow*> windows_in_category;
        for (EditorWindow* editor_window : editor_windows)
        {
            if (exclude_package_manager &&
                std::string(editor_window->m_Title) == EditorLayoutWindowIds::kPackageManager)
            {
                continue;
            }
            if (category_for_title(editor_window->m_Title) != category)
            {
                continue;
            }
            windows_in_category.push_back(editor_window);
        }

        if (windows_in_category.empty())
        {
            continue;
        }

        std::shared_ptr<ZSlate::SMenu> sub = parent.AddSubMenu(category_label(category), scale);
        for (EditorWindow* editor_window : windows_in_category)
        {
            sub->AddCheckItem(editor_window->m_Title, editor_window->m_Open,
                              [editor_window]() { editor_window->m_Open = !editor_window->m_Open; }, scale);
        }
    }
}

void EditorUI::ShowEditorWindowDock()
{
    if (m_EditorLayout != nullptr)
    {
        m_EditorLayout->OnGUI();
    }
}

bool EditorUI::Initialize()
{
    // The editor no longer creates an ImGui context on any platform: the dock
    // host, panels and overlay are native ZSlate, and the macOS render path
    // (EditorUIPassMacOS.mm) no longer rasterizes ImGui draw data either. Text
    // (including CJK) is rasterized by the native ZFontAtlas. The Profiler tool
    // still uses ImGui, but owns its own context independently.

    // setup window icon (stbi_* symbols are linked from ZRuntime's STB_IMAGE_IMPLEMENTATION TU)
    LoadGlfwWindowIcons(GET_SYSTEM(WindowSystem)->GetWindow(),
                        GET_SYSTEM(ConfigManager)->GetEditorBigIconPath().generic_string(),
                        GET_SYSTEM(ConfigManager)->GetEditorSmallIconPath().generic_string());
    RegisterAllEditorWindow();

    // Editor tear-off (true OS multi-window): the manager detaches panels into
    // their own swapchain-backed OS windows. Resolve the EditorUI now so menu /
    // drag entry points can request a float.
    FloatingPanelManager::Get().Initialize(this);

    // 创建 Editor 专属的 ImGui UI Pass
    auto render_system = GET_SYSTEM(RenderSystem);
    auto render_pipeline = render_system ? render_system->getRenderPipeline() : nullptr;
    if (render_pipeline == nullptr)
    {
        LOG_WARNING(ZEditor, "Render pipeline is unavailable; editor UI render pass is disabled.");
        GET_SYSTEM(WindowSystem)->ShowWindow();
        return true;
    }

    // 使用工厂模式创建 EditorUIPass
    m_EditorUiPass = CREATE_FROM_FACTORY(RenderPass, "EditorUIPass");
    if (m_EditorUiPass == nullptr)
    {
        LOG_WARNING(ZEditor, "Failed to create EditorUIPass; editor UI render pass is disabled.");
        GET_SYSTEM(WindowSystem)->ShowWindow();
        return true;
    }

    // 设置 common info
    RenderPassCommonInfo pass_common_info;
    pass_common_info.rhi = render_pipeline->GetRHI();
    pass_common_info.render_resource = render_pipeline->GetRenderResource();
    if (pass_common_info.rhi == nullptr)
    {
        LOG_WARNING(ZEditor, "RHI is unavailable; editor UI render pass is disabled.");
        m_EditorUiPass.reset();
        GET_SYSTEM(WindowSystem)->ShowWindow();
        return true;
    }
    m_EditorUiPass->SetCommonInfo(pass_common_info);

    // 初始化 EditorUIPass
    EditorUIPassInitInfo editor_ui_init_info;
    editor_ui_init_info.render_pass = render_pipeline->GetUIRenderPass();
    editor_ui_init_info.ui_layer_color_view = render_pipeline->GetUiLayerColorView();
    EditorUIPass* editor_ui_pass = AsEditorUIPass(m_EditorUiPass.get());
    editor_ui_pass->Initialize(&editor_ui_init_info);

    // 初始化 ImGui 渲染后端
    editor_ui_pass->InitializeUIRenderBackend(this);
    if (!editor_ui_pass->isBackendInitialized())
    {
        LOG_WARNING(ZEditor, "Failed to initialize editor UI render backend; editor UI render pass is disabled.");
        m_EditorUiPass.reset();
        GET_SYSTEM(WindowSystem)->ShowWindow();
        return true;
    }

    // Post-UI callback: editor ImGui draws after runtime UI.
    render_pipeline->registerPostUIRenderCallback([this]() {
        EditorUIPass* pass = AsEditorUIPass(m_EditorUiPass.get());
        if (pass != nullptr)
        {
            pass->Draw();
        }
    });

    render_pipeline->registerSkippedFrameCallback([editor_ui_pass]() {
        if (editor_ui_pass != nullptr)
        {
            editor_ui_pass->NotifySkippedRHIFrame();
        }
    });

    // Refresh RP2 UI layer RTV after framebuffer resize.
    render_pipeline->registerFramebufferRecreateCallback([editor_ui_pass, render_pipeline]() {
        if (editor_ui_pass != nullptr)
        {
            editor_ui_pass->RefreshUiLayerTarget(render_pipeline->GetUiLayerColorView());
        }
    });

    GET_SYSTEM(WindowSystem)->ShowWindow();
    return true;
}

void EditorUI::RegisterEditorWindow(const EditorWindowDescriptor& descriptor)
{
    if (!descriptor.factory || descriptor.dock_title == nullptr)
    {
        return;
    }

    EditorWindow* editor_window = descriptor.factory(this);
    if (editor_window == nullptr)
    {
        return;
    }

    editor_window->m_Open = descriptor.default_open;
    m_EditorWindows.insert(editor_window);
    if (descriptor.type_id != std::type_index(typeid(void)))
    {
        m_WindowMap[descriptor.type_id] = editor_window;
    }
}

void EditorUI::RegisterAllEditorWindow()
{
    EditorWindowRegistry::RegisterBuiltinWindows(*this);
}

void EditorUI::UnregisterAllEditorWindow()
{
    for (const auto& it : m_EditorWindows)
    {
        MemoryManager::DestroyObject(it);
    }
    m_EditorWindows.clear();
    m_WindowMap.clear();
}

std::vector<EditorWindow*> EditorUI::GetEditorWindows() const
{
    return {m_EditorWindows.begin(), m_EditorWindows.end()};
}

std::unordered_map<std::string, bool> EditorUI::GetEditorWindowOpenStates() const
{
    std::unordered_map<std::string, bool> states;
    for (const auto& editor_window : m_EditorWindows)
    {
        states[editor_window->m_Title] = editor_window->m_Open;
    }
    return states;
}

void EditorUI::ApplyEditorWindowOpenStates(const std::unordered_map<std::string, bool>& states)
{
    for (const auto& editor_window : m_EditorWindows)
    {
        auto iter = states.find(editor_window->m_Title);
        // Only touch windows explicitly listed in the layout snapshot. Newer panels
        // (e.g. Package Manager) must not be forced closed when absent from the map.
        if (iter != states.end())
        {
            editor_window->m_Open = iter->second;
        }
    }
}

EditorWindow* EditorUI::FindEditorWindow(const std::string& title) const
{
    for (const auto& editor_window : m_EditorWindows)
    {
        if (title == editor_window->m_Title)
        {
            return editor_window;
        }
    }
    return nullptr;
}

void EditorUI::UnregisterEditorWindow(EditorWindow* editor_window)
{
    const auto& iter = m_EditorWindows.find(editor_window);
    if (iter == m_EditorWindows.end())
        return;
    m_EditorWindows.erase(iter);
    m_WindowMap.clear();
}

void EditorUI::PreRender()
{
    ShowEditorUI();
}

void EditorUI::ProcessDeferredWork()
{
    if (ZSlateProjectWindow* project_window = GetWindow<ZSlateProjectWindow>())
    {
        project_window->ExecutePendingImportDialog();
    }

    // Scene/Hierarchy drag-drop must drain here (before Application::TickOneFrame
    // -> SwapLogicRenderData -> ProcessSwapData). If we only ExecutePendingDrop at
    // the tail of SceneWindow::onGUI, placement runs after RendererTick and the
    // new GameObjectDesc sits on the logic buffer until the next frame -- one frame
    // late and easy to mistake for "UpsertGameObject never runs" while debugging
    // ProcessSwapData in the same frame as the drop.
    EditorScenePlacement::ExecutePendingDrop();
}

void EditorUI::FinalizePendingImGuiPlatformFrame()
{
    if (!RenderingThread::IsParallelRenderingEnabled() || m_EditorUiPass == nullptr ||
        !m_ImGuiAwaitingPlatformUpdate)
    {
        return;
    }

    EditorUIPass* editor_ui_pass = AsEditorUIPass(m_EditorUiPass.get());
    if (editor_ui_pass == nullptr || !editor_ui_pass->isBackendInitialized())
    {
        m_ImGuiAwaitingPlatformUpdate = false;
        return;
    }

    if (!editor_ui_pass->WaitForImGuiRenderCompleteFor(std::chrono::milliseconds(500)))
    {
        LOG_WARNING(ZEditor,
                    "EditorUI: ImGui RHI frame timed out; releasing game-thread wait (skipped RHI draw?)");
        editor_ui_pass->NotifySkippedRHIFrame();
    }

    editor_ui_pass->ConsumeImGuiFrameRendered();
    m_ImGuiAwaitingPlatformUpdate = false;
}

void EditorUI::PrepareGameThreadImGuiFrame()
{
    if (!RenderingThread::IsParallelRenderingEnabled() || m_EditorUiPass == nullptr)
    {
        return;
    }

    EditorUIPass* editor_ui_pass = AsEditorUIPass(m_EditorUiPass.get());
    if (editor_ui_pass != nullptr && editor_ui_pass->isBackendInitialized())
    {
        editor_ui_pass->PrepareGameThreadFrame();
        m_ImGuiAwaitingPlatformUpdate = true;
    }
}
