#include "EditorApplication.h"

#include "Editor/Console/EditorConsoleCommands.h"
#include "Editor/RenderDoc/RenderDoc.h"
#include "Editor/EditorAsset/EditorAssetManager.h"
#include "Editor/EditorInputManager/EditorInputManager.h"
#include "Editor/EditorSceneManager/EditorSceneManager.h"
#include "Editor/EditorUI/EditorUI.h"
#include "Editor/FloatingPanel/FloatingPanelManager.h"
#include "Editor/Scripting/TypeScriptCompiler.h"
#include "Editor/Undo/EditorUndo.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"
#include "Runtime/Application/Application.h"
#include "Runtime/Core/Thread/ThreadManager.h"
#include "Runtime/Function/Command/CommandSystem.h"
#include "Runtime/Function/Console/ConsoleManager.h"
#include "Runtime/Function/Framework/GameFramework/GameEngine.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Function/Character/Character.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Input/InputSystem.h"
#include "Runtime/Function/Module/ModuleManager.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/RenderingThread/RenderingThread.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/Resource/Config/ConfigManager.h"
#include "Runtime/Scripting/ScriptRegistry.h"
#include "Runtime/Scripting/ScriptingManager.h"
#if defined(_WIN32)
    #include "Runtime/Function/Render/Interface/DX12/DX12RHI.h"
    #include "Runtime/Function/Render/Interface/RHI.h"
#endif
#include "Runtime/Resource/Asset/AssetManager.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#ifdef __APPLE__
    #include <limits.h>
    #include <mach-o/dyld.h>
    #include <unistd.h>
#endif

void registerEdtorTickComponent(std::string component_type_name)
{
    g_editorTickComponentTypes.insert(component_type_name);
}

std::vector<std::type_index> Editor::GetDependencies() const
{
    return {GET_SYSTEM_TYPE(Application), GET_SYSTEM_TYPE(PlayerSettings)};
}

bool Editor::Initialize()
{
    auto&& projectPath = GET_SYSTEM(CommandSystem)->GetProjectPath();
    if (projectPath.empty())
    {
        LOG_ERROR(ZEditor, "Project path is empty. Launcher dispatch should happen before editor initialization.");
        return false;
    }

    LOG_INFO(ZEditor, "Project path: {}", projectPath.generic_string());

    if (!std::filesystem::exists(projectPath))
    {
        LOG_ERROR(ZEditor, "Project path does not exist: {}", projectPath.generic_string());
        return false;
    }

    registerEdtorTickComponent("Transform");

    registerEdtorTickComponent("MeshRenderer");
    registerEdtorTickComponent("SkinMeshRenderer");
    // GPU-Spine：编辑模式也 tick ⇒ 推变换（拖 gizmo 时跟着动）+ 动画预览（spine-unity 编辑模式同样在动）
    registerEdtorTickComponent("SpineRenderer");

    // P5 TypeScript scripting: tick TypeScriptComponent in edit mode so
    // OnUpdate fires without needing a Play button (Unity [ExecuteAlways]
    // equivalent).
    registerEdtorTickComponent("TypeScriptComponent");

    // Load project library before engine initialization
    // This allows modules to register themselves during static initialization
    // before ModuleManager::InitializeAllModules() is called
    if (!GET_SYSTEM(ProjectInfo)->name.empty())
    {
        std::filesystem::path project_dir = std::filesystem::path(m_ProjectPath).parent_path();
        if (std::filesystem::is_directory(m_ProjectPath))
        {
            project_dir = m_ProjectPath;
        }
        LoadProjectLibrary(project_dir, GET_SYSTEM(ProjectInfo)->name.c_str());
    }

    GET_SYSTEM(EditorSceneManager)->setEditorCamera(GET_SYSTEM(RenderSystem)->GetRenderCamera(ViewportType::scene));
    GET_SYSTEM(EditorSceneManager)->UploadAxisResource();
    m_EditorUi = std::make_shared<EditorUI>();

    m_EditorUi->Initialize();

    // P10: native editor windowing + input layer. Subscribe to WindowSystem GLFW
    // callbacks on the main thread now that the window exists. Gated at the read
    // sites by r.ZSlate.NativeInput; the subscription itself is harmless when off.
    ZSlate::EditorSlateHost::Get().Initialize();

    if (auto console = GET_SYSTEM(ConsoleManager))
    {
        RegisterEditorConsoleCommands(*console);
    }

    Runtime::RenderDoc::Init();

#if defined(_WIN32)
    // IBL cubemap GPU upload runs during RenderSystem::Initialize, before ConsoleWindow
    // registers its log callback. Re-emit the summary here so it appears in the Console.
    if (auto render_system = GET_SYSTEM(RenderSystem))
    {
        if (auto* dx12_rhi = dynamic_cast<DX12RHI*>(render_system->GetRHI()))
        {
            dx12_rhi->LogDeferredIblCubemapDiagnostics();
        }
    }
#endif

    // Enable shared memory for profiler communication
    Profiler::GetInstance().EnableSharedMemory(true);

    // UE-style live resize: keep presenting during the Win32 modal resize/move loop (border drag),
    // when glfwPollEvents blocks and Run() is frozen. Without this the FLIP swapchain shows white
    // for the un-presented grown area until the drag is released.
    GET_SYSTEM(WindowSystem)->RegisterOnWindowRefreshFunc([this]() { RenderFrameDuringResize(); });

    // DX12 swapchain recovery on Alt-Tab return: FLIP-model swapchains can enter an
    // occluded / device-lost state when the window is in the background. Notify the
    // RHI so PrepareBeforePass forces a full swapchain recreate on the next frame.
    // Vulkan handles this reactively via VK_ERROR_OUT_OF_DATE_KHR, so its implementation
    // is a no-op — but calling it unconditionally keeps the call site simple.
    if (auto* rhi = GET_SYSTEM(RHI))
    {
        GET_SYSTEM(WindowSystem)->RegisterOnWindowFocusFunc([rhi](int focused) {
            if (focused)
            {
                rhi->NotifyWindowFocusGained();
            }
        });
    }

    // Show window immediately after initialization to avoid delay
    GET_SYSTEM(WindowSystem)->ShowWindow();
    SetPlaybackState(EditorPlaybackState::Editing);

    // Wire P3 (TypeScriptCompiler) -> P4 (ScriptingManager): when tsc emits
    // a fresh .js, ScriptingManager loads (or reloads) the corresponding
    // cached module. We debounce on the calling thread because Win32
    // ReadDirectoryChangesW emits multiple events (ADD then MODIFIED)
    // for a single editor save, and tsc itself sometimes overwrites the
    // file in two passes (write-then-rename).
    if (auto tsc = GET_SYSTEM(TypeScriptCompiler))
    {
        auto scripting = GET_SYSTEM(ScriptingManager);
        auto registry_sptr = GET_SYSTEM(ScriptRegistry);
        // Capture a raw pointer in the lambda: the engine owns ScriptRegistry's
        // lifetime via its system list and shutdown happens AFTER the watcher
        // is torn down, so the raw pointer is safe for the lifetime of the
        // bridge. Avoids copying a shared_ptr per file event.
        ScriptRegistry* registry = registry_sptr;
        tsc->SetOnJsModuleChanged([scripting, registry](const std::filesystem::path& abs_js_path) {
            if (!scripting)
                return;
            // Refresh the matching ScriptAsset's m_CompiledRelPath BEFORE
            // we touch the loader. Without this, a TypeScriptComponent that
            // attaches before tsc has emitted the .js will permanently fail
            // BindAndAwake because the registry entry never learns about
            // the eventual compiled output -- only rescan() (which only
            // runs at startup) updates that field. The Inspector then
            // shows "(waiting for tsc to emit X.js)" forever, even after
            // the .js is on disk.
            if (registry)
                registry->OnCompiledJsChanged(abs_js_path);
            // Debounce identical (module_id, mtime) pairs that arrive within
            // 200ms of each other - that's roughly the window in which Windows
            // duplicates a single save.
            static std::mutex s_Mutex;
            static std::unordered_map<std::string,
                                      std::chrono::steady_clock::time_point>
                s_LastSeen;
            const std::string module_id = scripting->PathToModuleId(abs_js_path);
            if (module_id.empty())
                return;
            const auto now = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lk(s_Mutex);
                auto it = s_LastSeen.find(module_id);
                if (it != s_LastSeen.end() && now - it->second < std::chrono::milliseconds(200))
                {
                    it->second = now;
                    return;
                }
                s_LastSeen[module_id] = now;
            }
            // First sighting -> LoadModule. Subsequent sightings of the
            // same module -> ReloadModule. This way scripts get picked up
            // automatically on first compile, and editor-time saves
            // hot-reload them without any explicit load call from the
            // hierarchy / inspector.
            if (scripting->IsModuleLoaded(module_id))
            {
                LOG_INFO(ZScripting, "hot-reloading module: {}", module_id);
                scripting->ReloadModule(module_id);
            }
            else
            {
                LOG_INFO(ZScripting, "auto-loading module: {}", module_id);
                if (scripting->LoadModule(module_id))
                {
                    // First-time load doesn't fire the reload-observer chain
                    // (the cache transition is absent->present, not
                    // present->fresh), but TypeScriptComponents that tried
                    // to bind before tsc finished compiling are sitting on
                    // the chain waiting for exactly this signal -- they
                    // subscribed in BindAndAwake's failure path and will
                    // retry on notification. Fire it so they wake up.
                    scripting->NotifyModuleReloaded(module_id);
                }
            }
        });
        LOG_INFO(ZScripting, "TypeScriptCompiler -> ScriptingManager hot-reload bridge wired");

        // Pick up modules that tsc compiled in a previous editor session
        // (i.e. .js files that already exist when the editor starts and
        // that tsc may not re-emit if they're already up-to-date with
        // their .ts source). Without this, a clean restart on a project
        // with no source changes would never load any module until the
        // user touches a .ts file.
        if (scripting)
        {
            auto js_root_path = std::filesystem::path(scripting->GetJsRoot());
            if (!js_root_path.empty())
            {
                std::error_code ec;
                if (std::filesystem::exists(js_root_path, ec))
                {
                    for (auto& entry : std::filesystem::recursive_directory_iterator(js_root_path, ec))
                    {
                        if (ec)
                            break;
                        if (!entry.is_regular_file())
                            continue;
                        const auto& p = entry.path();
                        // Only top-level .js files - skip .js.map and
                        // anything under nested node_modules-like dirs.
                        auto ext = p.extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
                        if (ext != ".js")
                            continue;
                        const std::string module_id = scripting->PathToModuleId(p);
                        if (module_id.empty())
                            continue;
                        if (scripting->IsModuleLoaded(module_id))
                            continue;
                        LOG_INFO(ZScripting, "startup-loading existing module: {}", module_id);
                        scripting->LoadModule(module_id);
                    }
                }
            }
        }
    }
    return true;
}

void Editor::Shutdown()
{
    // Disable shared memory before shutdown
    Profiler::GetInstance().EnableSharedMemory(false);

    // Shutdown game modules before unloading the library
    // This ensures all module resources are released before DLL unload
    LOG_INFO(ZEditor, "Shutting down game modules before unloading project library...");
    GET_SYSTEM(ModuleManager)->ShutdownAllModules();

    // Give threads a moment to fully exit and complete any cleanup
    // This helps ensure DLL_PROCESS_DETACH handlers can complete without blocking
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Unload project library
    UnloadProjectLibrary();

    m_EditorUi.reset();
}

bool Editor::LoadProjectLibrary(const std::filesystem::path& project_path, const eastl::string& project_name)
{
    if (project_name.empty())
    {
        LOG_WARNING(ZEditor, "Cannot load project library: project name is empty");
        return false;
    }

    // Try to find the built library in common build output directories
    // On Windows: Debug/Release folders, on Linux: build directories
    std::vector<std::filesystem::path> search_paths;

    // Common build output paths
    search_paths.push_back(project_path / "build" / "Debug" / "source" / project_name.c_str());
    search_paths.push_back(project_path / "build" / "Release" / "source" / project_name.c_str());
    search_paths.push_back(project_path / "build" / "RelWithDebInfo" / "source" / project_name.c_str());
    search_paths.push_back(project_path / "build" / "MinSizeRel" / "source" / project_name.c_str());
    search_paths.push_back(project_path / "build" / "source" / project_name.c_str());
    search_paths.push_back(project_path / "bin" / "Debug" / "source" / project_name.c_str());
    search_paths.push_back(project_path / "bin" / "Release" / "source" / project_name.c_str());
    search_paths.push_back(project_path / "bin" / "source" / project_name.c_str());

    // Also try unified output directory (if ZENGINE_UNIFIED_OUTPUT_DIR is used)
    const char* engine_root = std::getenv("ZENGINE_ROOT");
    if (engine_root)
    {
        std::filesystem::path unified_dir = std::filesystem::path(engine_root) / "bin" / "source" / project_name.c_str();
        search_paths.push_back(unified_dir);
    }

    // Construct library filename based on platform
    eastl::string lib_name;
#ifdef _WIN32
    lib_name = project_name + ".dll";
#elif __APPLE__
    lib_name = "lib" + project_name + ".dylib";
#else
    lib_name = "lib" + project_name + ".so";
#endif

    std::filesystem::path library_path;
    bool found = false;

    for (const auto& search_path : search_paths)
    {
        std::filesystem::path candidate = search_path / lib_name.c_str();
        if (std::filesystem::exists(candidate))
        {
            library_path = candidate;
            found = true;
            break;
        }
    }

    if (!found)
    {
        LOG_WARNING(ZEditor, "Project library not found. Searched in:\n"
                             "  - {}/build/*/source/{}\n"
                             "  - {}/bin/*/source/{}\n"
                             "Please build the project first.",
                    project_path.generic_string(),
                    project_name.c_str(),
                    project_path.generic_string(),
                    project_name.c_str());
        return false;
    }

    // Convert to absolute path to ensure LoadLibrary can find dependencies
    std::filesystem::path absolute_library_path = std::filesystem::absolute(library_path);

    // Load the library
#ifdef _WIN32
    // Use LoadLibraryA with UTF-8 path converted to ANSI
    // Note: LoadLibraryA expects the system codepage, but we'll use the path string directly
    // For better Unicode support, we could use LoadLibraryW with proper conversion
    std::string path_str = absolute_library_path.string();
    m_ProjectLibraryHandle = LoadLibraryA(path_str.c_str());
    if (!m_ProjectLibraryHandle)
    {
        DWORD error = GetLastError();
        LOG_ERROR(ZEditor,
                  "Failed to load project library '{}': Error code {}",
                  absolute_library_path.generic_string(),
                  error);
        return false;
    }
#else
    std::string path_str = absolute_library_path.string();
    m_ProjectLibraryHandle = dlopen(path_str.c_str(), RTLD_LAZY | RTLD_GLOBAL);
    if (!m_ProjectLibraryHandle)
    {
        const char* error = dlerror();
        LOG_ERROR(ZEditor,
                  "Failed to load project library '{}': {}",
                  absolute_library_path.generic_string(),
                  error ? error : "Unknown error");
        return false;
    }
#endif

    // Call InitializeLibrary function to register the module
    // This avoids using static variables which can prevent DLL unloading
    typedef bool (*InitializeLibraryFunc)();
#ifdef _WIN32
    InitializeLibraryFunc init_func =
        (InitializeLibraryFunc)GetProcAddress(m_ProjectLibraryHandle, "InitializeLibrary");
#else
    InitializeLibraryFunc init_func = (InitializeLibraryFunc)dlsym(m_ProjectLibraryHandle, "InitializeLibrary");
#endif

    if (init_func)
    {
        if (!init_func())
        {
            LOG_ERROR(ZEditor, "Failed to initialize project library");
#ifdef _WIN32
            FreeLibrary(m_ProjectLibraryHandle);
#else
            dlclose(m_ProjectLibraryHandle);
#endif
            m_ProjectLibraryHandle = nullptr;
            return false;
        }
        LOG_INFO(ZEditor, "Successfully initialized project library");
    }
    else
    {
        LOG_WARNING(
            ZEditor,
            "InitializeLibrary function not found in project library. The library may use static initialization.");
    }

    LOG_INFO(ZEditor, "Successfully loaded project library: {}", library_path.generic_string());
    return true;
}

void Editor::UnloadProjectLibrary()
{
    if (m_ProjectLibraryHandle)
    {
        // Call UninitializeLibrary function before unloading
        // This ensures proper cleanup and avoids issues with static destructors
        typedef void (*UninitializeLibraryFunc)();
#ifdef _WIN32
        UninitializeLibraryFunc uninit_func =
            (UninitializeLibraryFunc)GetProcAddress(m_ProjectLibraryHandle, "UninitializeLibrary");
#else
        UninitializeLibraryFunc uninit_func =
            (UninitializeLibraryFunc)dlsym(m_ProjectLibraryHandle, "UninitializeLibrary");
#endif

        if (uninit_func)
        {
            uninit_func();
            LOG_INFO(ZEditor, "Successfully uninitialized project library");
        }
        else
        {
            LOG_WARNING(ZEditor,
                        "UninitializeLibrary function not found in project library. The library may use static "
                        "initialization.");
        }

        // LOG_INFO(ZEditor, "Unloading project library...");
#ifdef _WIN32
        // FreeLibrary can hang if DLL's DllMain blocks during DLL_PROCESS_DETACH
        // This usually happens if:
        // 1. DLL has active threads that haven't exited
        // 2. DLL's static destructors are blocking
        // 3. DLL is waiting on resources that are already destroyed
        // By calling UninitializeLibrary first, we avoid most static destructor issues
        BOOL result = FreeLibrary(m_ProjectLibraryHandle);
        // if (!result)
        //{
        //     DWORD error = GetLastError();
        //     //LOG_WARNING(ZEditor, "FreeLibrary failed with error code: {}. The library may still be in use.",
        //     error);
        // }
        // else
        //{
        //     //LOG_INFO(ZEditor, "Successfully unloaded project library");
        // }
#else
        int result = dlclose(m_ProjectLibraryHandle);
        if (result != 0)
        {
            const char* error = dlerror();
            LOG_WARNING(
                ZEditor, "dlclose failed: {}. The library may still be in use.", error ? error : "Unknown error");
        }
        else
        {
            LOG_INFO(ZEditor, "Successfully unloaded project library");
        }
#endif
        m_ProjectLibraryHandle = nullptr;
    }
}

void Editor::TogglePlayMode()
{
    SetPlaybackState(isInEditMode() ? EditorPlaybackState::Playing : EditorPlaybackState::Editing);
}

void Editor::TogglePauseMode()
{
    if (isInEditMode())
    {
        return;
    }

    SetPlaybackState(isPaused() ? EditorPlaybackState::Playing : EditorPlaybackState::Paused);
}

void Editor::RequestStepFrame()
{
    if (isInEditMode())
    {
        return;
    }

    if (!isPaused())
    {
        SetPlaybackState(EditorPlaybackState::Paused);
    }

    m_StepFrameRequested = true;
}

const char* Editor::GetPlaybackStateLabel() const
{
    switch (m_PlaybackState)
    {
        case EditorPlaybackState::Editing:
            return "Edit Mode";
        case EditorPlaybackState::Playing:
            return "Play Mode";
        case EditorPlaybackState::Paused:
            return "Paused";
        default:
            return "Edit Mode";
    }
}

void Editor::SetPlaybackState(EditorPlaybackState new_state)
{
    if (m_PlaybackState == new_state)
    {
        return;
    }

    const EditorPlaybackState old_state = m_PlaybackState;
    m_PlaybackState = new_state;
    m_StepFrameRequested = false;
    g_isPlaying = (new_state != EditorPlaybackState::Editing);

    // 游戏层随"是否正在播放"建立与拆除 —— 之前这里只改了一个枚举和
    // g_isPlaying，GameInstance / LocalPlayer / PlayerController 从未在编辑器里
    // 存在过，所以 F5 之后除了输入被屏蔽，什么也没发生。
    const bool was_playing = (old_state != EditorPlaybackState::Editing);
    if (!was_playing && g_isPlaying)
    {
        EnterPIE();
    }
    else if (was_playing && !g_isPlaying)
    {
        ExitPIE();
    }

    GET_SYSTEM(EditorInputManager)->resetEditorCommand();
    GET_SYSTEM(InputSystem)->resetGameCommand();
    GET_SYSTEM(WindowSystem)->SetFocusMode(false);
    GET_SYSTEM(EditorSceneManager)->DrawSelectedEntityAxis();

    LOG_INFO(ZEditor, "Editor playback state switched to {}", GetPlaybackStateLabel());
}

void Editor::EnterPIE()
{
    auto* game_engine = GET_SYSTEM(GameEngine);
    if (game_engine == nullptr)
    {
        LOG_ERROR(ZEditor, "EnterPIE: GameEngine is not registered; PIE will run without the game layer");
        return;
    }

    if (!game_engine->BeginPIE())
    {
        LOG_ERROR(ZEditor, "EnterPIE: BeginPIE failed; falling back to edit mode");
        // Roll the state back so the UI does not claim to be playing while the
        // game layer is absent. Recursive state change is safe: the transition
        // is now Playing -> Editing, so EnterPIE is not re-entered.
        SetPlaybackState(EditorPlaybackState::Editing);
    }
}

void Editor::ExitPIE()
{
    // 必须在 EndPIE() 之前取：EndPIE 会在丢弃 Play World 后把这个标志清掉，
    // 而它正是"要不要重载编辑器关卡"的唯一判据。
    bool used_play_world = false;
    auto* game_engine = GET_SYSTEM(GameEngine);
    if (game_engine != nullptr)
    {
        used_play_world = game_engine->IsPlayWorldPIE();
        game_engine->EndPIE();
    }

    // 走 WorldManager 而不是 EditorSceneManager::OpenSceneInternal：后者会弹
    // "未保存的改动"提示，而 PIE 的改动永远是要丢弃的，不是要保存的。
    auto* world_manager = GET_SYSTEM(WorldManager);
    if (world_manager == nullptr)
    {
        return;
    }

    // 只有退化成同世界播放时才需要重载：那种情况下游戏逻辑直接改过编辑器
    // 打开的那份关卡（角色被移动、组件状态被改），重载是丢弃改动的唯一廉价
    // 办法。独立 Play World 下编辑器世界从未被碰过，重载反而会白白丢掉用户
    // 在 PIE 之前做的未保存改动。
    if (!used_play_world)
    {
        GET_SYSTEM(RenderSystem)->ClearForLevelReloading();
        if (!world_manager->ReloadActiveLevelInPlace())
        {
            LOG_WARNING(ZEditor, "ExitPIE: could not reload the level; runtime changes may persist");
        }
    }

    GET_SYSTEM(EditorSceneManager)->DrawSelectedEntityAxis();
    GET_SYSTEM(EditorSceneManager)->RefreshMainWindowTitle();
}

bool Editor::ResolveSimulationTick(float frame_delta_time, float& simulation_delta_time)
{
    simulation_delta_time = frame_delta_time;

    switch (m_PlaybackState)
    {
        case EditorPlaybackState::Editing:
        case EditorPlaybackState::Playing:
            return true;
        case EditorPlaybackState::Paused:
            if (m_StepFrameRequested)
            {
                m_StepFrameRequested = false;
                return true;
            }
            simulation_delta_time = 0.0f;
            return false;
        default:
            return true;
    }
}

void Editor::Run(float max_seconds)
{
    float delta_time;
    auto& profiler = Profiler::GetInstance();

    // Unattended smoke runs (`--pie <seconds>`) need a wall-clock deadline:
    // there is no window message to close on, and the only way out of the loop
    // otherwise would be killing the process, which skips shutdown/ordering.
    const bool has_deadline = max_seconds > 0.0f;
    const std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();

    while (true)
    {
        if (has_deadline)
        {
            const float elapsed =
                std::chrono::duration<float>(std::chrono::steady_clock::now() - start_time).count();
            // 两个计时起算点不同：deadline 从进程起算，而 --pie-stop 从"进入
            // 播放态"起算（关卡加载本身要花掉一段时间），所以同为一个 N 秒时，
            // stop 一定晚于 deadline —— 直接比会让停止路径永远跑不到。
            // 停止还挂着就先别退出，给它一段宽限；宽限用满仍没停就直接退出，
            // 避免卡死。
            const bool awaiting_pie_stop = m_PieStarted && m_PieStopRequested;
            const float grace = awaiting_pie_stop ? max_seconds : 0.0f;
            if (elapsed >= max_seconds + grace)
            {
                if (awaiting_pie_stop)
                {
                    LOG_WARNING(ZEditor, "--pie-stop: grace expired without leaving play mode; exiting anyway");
                }
                LOG_INFO(ZEditor, "Editor run deadline reached ({:.1f}s); exiting", max_seconds);
                break;
            }
        }

        profiler.NextFrame();

        {
            Z_PROFILE_SCOPE("Editor::run");

            delta_time = GET_SYSTEM(Application)->CalculateDeltaTime();
            float simulation_delta_time = delta_time;
            const bool should_update_logic = ResolveSimulationTick(delta_time, simulation_delta_time);

            {
                Z_PROFILE_SCOPE("SceneManager::tick");
                GET_SYSTEM(EditorSceneManager)->Tick(delta_time);
            }

            // `--pie <seconds>`: the request was made before the startup level
            // existed, so enter play mode here -- after SceneManager::Tick has
            // opened the level and after the world is confirmed loaded. Doing it
            // in START_SYSTEM() made BeginPIE fail with "no active level".
            if (m_PieSeconds > 0.0f && !m_PieStarted)
            {
                if (auto* world_manager = GET_SYSTEM(WorldManager);
                    world_manager != nullptr && world_manager->IsWorldLoaded() &&
                    world_manager->getCurrentActiveLevel() != nullptr)
                {
                    m_PieStarted = true;
                    m_PieStartClock = std::chrono::steady_clock::now();
                    LOG_INFO(ZEditor, "--pie: level ready, entering play mode for {:.1f}s", m_PieSeconds);
                    TogglePlayMode();
                }
            }

            // `--pie-move-test`: drive the character for real. An unattended PIE
            // run has no keyboard, so InputSystem reports command 0 every frame,
            // the character never moves, and the run "passes" without ever
            // executing CharacterController::move(). Forcing the command makes
            // the whole pipeline run: MotorComponent -> Controller::move ->
            // PhysicsScene::IsOverlap. The displacement is asserted at the end,
            // so a controller that is misconfigured (ControllerType::invalid, or
            // a null capsule) fails loudly instead of looking green.
            if (m_PieMoveTest && m_PieStarted && isPlaying())
            {
                if (InputSystem* input_system = GET_SYSTEM(InputSystem))
                {
                    input_system->SetForcedTestCommand(static_cast<unsigned int>(GameCommand::forward));
                }

                if (!m_PieMoveTestPositionCaptured)
                {
                    if (Vector3 start_position; TryGetActiveCharacterPosition(start_position))
                    {
                        m_PieMoveTestStartPosition = start_position;
                        m_PieMoveTestPositionCaptured = true;
                        LOG_INFO(ZEditor,
                                 "--pie-move-test: start position ({:.3f}, {:.3f}, {:.3f})",
                                 start_position.x,
                                 start_position.y,
                                 start_position.z);
                    }
                }
            }

    // `--pie-stop`: leave play mode after the play phase has run, so the
    // smoke test exercises EndPIE (Play World teardown / level restore)
    // instead of just killing the process at the deadline. The stop runs
    // before the deadline check so the remaining time still lets the
    // stop-path frames tick.
    if (m_PieStarted && m_PieStopRequested && isPlaying())
    {
        const float played =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - m_PieStartClock).count();
        if (played >= m_PieSeconds)
        {
            // 断言必须在这里取样，不能等到 ReportPieMoveTestResult()：
            // TogglePlayMode 一执行，EndPIE 就把 Play World 整个丢掉，
            // getCurrentActiveLevel() 随即变回编辑器世界 —— 那时读到的角色
            // 是编辑器里那份（从未移动过），位移恒为 0，测试报的是假失败。
            if (m_PieMoveTest && m_PieMoveTestPositionCaptured)
            {
                if (Vector3 end_position; TryGetActiveCharacterPosition(end_position))
                {
                    m_PieMoveTestEndPosition = end_position;
                    m_PieMoveTestEndCaptured = true;
                }
            }

            LOG_INFO(ZEditor, "--pie-stop: played {:.1f}s, leaving play mode", played);
            m_PieStopRequested = false;
            TogglePlayMode();
        }
    }


            {
                Z_PROFILE_SCOPE("InputManager::tick");
                GET_SYSTEM(EditorInputManager)->Tick(delta_time);
            }
            {
                // Closes last frame's implicit undo group (every edit made by one UI
                // event without an explicit transaction becomes one undo step), and
                // drops the history when the level it refers to was replaced.
                Z_PROFILE_SCOPE("EditorUndoSystem::Tick");
                if (auto undo = GET_SYSTEM(EditorUndoSystem))
                    undo->Tick();
            }
            {
                // P9: pump the AssetRegistry's FileSystemWatcher event queue
                // on the main thread. Watcher events arrive on a worker
                // thread (Win32 ReadDirectoryChangesW etc.) and are buffered
                // until this drain runs. Cheap when the queue is empty.
                Z_PROFILE_SCOPE("EditorAssetManager::TickWatcher");
                if (auto eam = dynamic_cast<EditorAssetManager*>(GET_SYSTEM(AssetManager)))
                    eam->TickWatcher();
            }
            {
                Z_PROFILE_SCOPE("TypeScriptCompiler::tick");
                if (auto tsc = GET_SYSTEM(TypeScriptCompiler))
                    tsc->Tick();
            }
            {
                Z_PROFILE_SCOPE("ScriptingManager::tick");
                if (auto sm = GET_SYSTEM(ScriptingManager))
                    sm->Tick();
            }
            if (m_EditorUi)
            {
                Z_PROFILE_SCOPE("EditorUI::ProcessDeferredWork");
                m_EditorUi->ProcessDeferredWork();
            }
            if (m_EditorUi)
            {
                Z_PROFILE_SCOPE("EditorUI::FinalizePendingImGuiPlatformFrame");
                m_EditorUi->FinalizePendingImGuiPlatformFrame();
            }
            {
                // Editor tear-off: create/destroy/resize floating panel OS windows
                // on the main thread (GLFW requirement) before UI batches are built.
                Z_PROFILE_SCOPE("FloatingPanelManager::TickMainThread");
                FloatingPanelManager::Get().TickMainThread();
            }
            if (m_EditorUi)
            {
                Z_PROFILE_SCOPE("EditorUI::PrepareGameThreadImGuiFrame");
                m_EditorUi->PrepareGameThreadImGuiFrame();
            }
            Runtime::RenderDoc::OnPreFrame();
            {
                Z_PROFILE_SCOPE("Engine::tickOneFrame");
                if (!GET_SYSTEM(Application)->TickOneFrame(simulation_delta_time, delta_time, should_update_logic))
                    return;
            }
            Runtime::RenderDoc::OnPostFrame();

            {
                // 游戏层（GameInstance → LocalPlayer → PlayerController）。
                // 放在 TickOneFrame 之后，与独立进程 GameEngine::Run() 的顺序
                // 一致：本帧的输入与物理已经推进过，游戏层读到的是本帧数据。
                // Paused 时 should_update_logic 为 false（单帧步进除外），
                // 因此这里也要跟着跳过，否则暂停了角色还在动。
                Z_PROFILE_SCOPE("GameEngine::tick");
                if (should_update_logic)
                {
                    if (auto* game_engine = GET_SYSTEM(GameEngine); game_engine != nullptr)
                    {
                        game_engine->Tick(simulation_delta_time);
                    }
                }
            }
        }
    }

    // Unattended movement validation (`--pie-move-test`) reports here, after
    // the loop has wound down but before the process exits, so a controller that
    // never moved is visible in the log instead of being silently green.
    ReportPieMoveTestResult();
    // The log ring buffer is only drained by the debug-layer pump on the next
    // frame; with the run loop already exited that never happens, so the verdict
    // would stay in memory and the unattended run would end with no verdict at
    // all (seen as "PIE stopped" but no PASSED/FAILED line). Flush explicitly.
    ZEngineForceFlushLogs();
}

bool Editor::TryGetActiveCharacterPosition(Vector3& out_position) const
{
    const WorldManager* world_manager = GET_SYSTEM(WorldManager);
    if (world_manager == nullptr)
    {
        return false;
    }

    Level* level = world_manager->getCurrentActiveLevel();
    if (level == nullptr)
    {
        return false;
    }

    std::shared_ptr<Character> character = level->getCurrentActiveCharacter().lock();
    if (character == nullptr)
    {
        return false;
    }

    std::shared_ptr<GameObject> character_object = character->getObject().lock();
    if (character_object == nullptr)
    {
        return false;
    }

    const Transform* transform_component = character_object->tryGetComponentConst(Transform);
    if (transform_component == nullptr)
    {
        return false;
    }

    out_position = transform_component->GetLocalPosition();
    return true;
}

void Editor::ReportPieMoveTestResult()
{
    if (!m_PieMoveTest)
    {
        return;
    }

    if (InputSystem* input_system = GET_SYSTEM(InputSystem))
    {
        input_system->SetForcedTestCommand(0u);
    }

    if (!m_PieMoveTestPositionCaptured)
    {
        LOG_ERROR(ZEditor, "--pie-move-test: FAILED -- no active character, pipeline never ran");
        m_PieMoveTestFailed = true;
        return;
    }

    // 终点已在退出播放态之前取样（见 --pie-stop 分支）：EndPIE 之后 Play World
    // 就没了，那时再读只会拿到编辑器世界那份从未移动过的角色。
    if (!m_PieMoveTestEndCaptured)
    {
        LOG_ERROR(ZEditor, "--pie-move-test: FAILED -- end position never captured; PIE ended too early");
        m_PieMoveTestFailed = true;
        return;
    }

    const Vector3 delta = m_PieMoveTestEndPosition - m_PieMoveTestStartPosition;
    const float travelled = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);

    if (travelled > kPieMoveTestMinDistance)
    {
        LOG_INFO(ZEditor,
                 "--pie-move-test: PASSED -- character moved {:.3f} to ({:.3f}, {:.3f}, {:.3f})",
                 travelled,
                 m_PieMoveTestEndPosition.x,
                 m_PieMoveTestEndPosition.y,
                 m_PieMoveTestEndPosition.z);
        return;
    }

    LOG_ERROR(ZEditor,
              "--pie-move-test: FAILED -- character moved only {:.6f} (needs > {:.3f}). "
              "Start ({:.3f}, {:.3f}, {:.3f}) end ({:.3f}, {:.3f}, {:.3f})",
              travelled,
              kPieMoveTestMinDistance,
              m_PieMoveTestStartPosition.x,
              m_PieMoveTestStartPosition.y,
              m_PieMoveTestStartPosition.z,
              m_PieMoveTestEndPosition.x,
              m_PieMoveTestEndPosition.y,
              m_PieMoveTestEndPosition.z);
    m_PieMoveTestFailed = true;
}

void Editor::RenderFrameDuringResize()
{
    // Fired by WindowSystem's window-refresh callback, which GLFW dispatches from inside
    // glfwPollEvents -- including the WM_PAINT messages delivered during the Win32 modal
    // resize/move loop, when the normal Run() loop is blocked in PollEvents and cannot present.
    //
    // This mirrors UE's FSlateApplication behaviour during a resize: render ONE frame
    // SYNCHRONOUSLY. The earlier naive version re-entered the async parallel pipeline mid-frame
    // and deadlocked the ImGui game/render handshake (maximize flicker + permanent white on
    // shrink). The fix is to bracket the frame with FlushRenderingCommands(): drain the in-flight
    // async frame first, render+submit our resize frame, then drain again so it is fully presented
    // before we return. No two frames are ever in flight, so the handshake stays balanced.
    if (m_InResizeRender || m_EditorUi == nullptr || !RenderingThread::IsParallelRenderingEnabled())
    {
        return;
    }

    auto render_system = GET_SYSTEM(RenderSystem);
    if (render_system == nullptr)
    {
        return;
    }

    m_InResizeRender = true;

    // 1. Drain whatever async frame Run()'s last RendererTick dispatched (it is mid-flight on the
    //    render/RHI threads right now). After this the pipeline is idle.
    render_system->FlushRenderingCommands();

    // 2. Consume the in-flight frame's ImGui handshake (its Draw already signalled completion during
    //    the Flush), then build a fresh ImGui frame at the current (live) window size.
    m_EditorUi->FinalizePendingImGuiPlatformFrame();
    m_EditorUi->PrepareGameThreadImGuiFrame();

    // 3. Push the resize frame through the pipeline. PrepareBeforePass sees the framebuffer/extent
    //    delta and recreates the swapchain, then renders + presents at the new size.
    render_system->SwapLogicRenderData();
    render_system->Tick(0.0f);

    // 4. Drain again so the resize frame is actually presented before this callback returns -- this
    //    is what makes the window track the drag live instead of showing white.
    render_system->FlushRenderingCommands();

    m_InResizeRender = false;
}