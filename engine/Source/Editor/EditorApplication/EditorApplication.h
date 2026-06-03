#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Function/PlayerSettings/PlayerSettings.h"
#include "Runtime/Project/ProjectInfo.h"

#include <cstdint>
#include <filesystem>
#include <memory>

enum class EditorPlaybackState : uint8_t
{
    Editing,
    Playing,
    Paused
};

#ifdef _WIN32

    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

class EditorUI;

class Editor : public IEngineSystem

{
    friend class EditorUI;

public:
    std::string GetName() const override { return GET_CLASS_NAME(Editor); }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PostInit; }
    std::vector<std::type_index> GetDependencies() const override;
    bool Initialize() override;
    void Shutdown() override;
    void Run();
    bool isInEditMode() const { return m_PlaybackState == EditorPlaybackState::Editing; }
    bool isPlaying() const { return m_PlaybackState != EditorPlaybackState::Editing; }
    bool isPaused() const { return m_PlaybackState == EditorPlaybackState::Paused; }
    void TogglePlayMode();
    void TogglePauseMode();
    void RequestStepFrame();
    const char* GetPlaybackStateLabel() const;

protected:
    /**
     * @brief Load project library dynamically
     * @param project_path Path to project directory
     * @param project_name Project name (module name)
     * @return true if loaded successfully, false otherwise
     */
    bool LoadProjectLibrary(const std::filesystem::path& project_path, const eastl::string& project_name);

    /**
     * @brief Unload project library
     */
    void UnloadProjectLibrary();

private:
    void SetPlaybackState(EditorPlaybackState new_state);
    bool ResolveSimulationTick(float frame_delta_time, float& simulation_delta_time);
    // Renders one frame at the live window size from inside the Win32 modal resize/move loop
    // (driven by WindowSystem's window-refresh callback while Run() is blocked in glfwPollEvents).
    // UE-style: the parallel pipeline is drained (FlushRenderingCommands) before AND after so the
    // resize frame is fully synchronous and never overlaps the async render/RHI threads -- this is
    // what keeps it from wedging the ImGui game/render handshake the way a naive async re-entry did.
    void RenderFrameDuringResize();

    std::shared_ptr<EditorUI> m_EditorUi;
    std::string m_ProjectPath;
    EditorPlaybackState m_PlaybackState {EditorPlaybackState::Editing};
    bool m_StepFrameRequested {false};
    // Re-entrancy guard for RenderFrameDuringResize() (the refresh callback can fire recursively
    // when nested message pumps deliver WM_PAINT during the Win32 modal resize/move loop).
    bool m_InResizeRender {false};

#ifdef _WIN32
    HMODULE m_ProjectLibraryHandle {nullptr};
#else
    void* m_ProjectLibraryHandle {nullptr};
#endif
};
