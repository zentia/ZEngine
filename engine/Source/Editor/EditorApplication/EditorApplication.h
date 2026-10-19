#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Math/Vector3.h"
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

    /// 主循环。max_seconds <= 0 表示一直跑到窗口关闭（正常编辑器行为）；
    /// > 0 时到点自动退出，供 `--pie` 无人值守冒烟测试使用。
    void Run(float max_seconds = 0.0f);
    bool isInEditMode() const { return m_PlaybackState == EditorPlaybackState::Editing; }
    bool isPlaying() const { return m_PlaybackState != EditorPlaybackState::Editing; }
    bool isPaused() const { return m_PlaybackState == EditorPlaybackState::Paused; }
    void TogglePlayMode();
    void TogglePauseMode();
    void RequestStepFrame();
    const char* GetPlaybackStateLabel() const;
    /// 无人值守冒烟测试（`--pie <seconds>`）请求的播放。
    /// START_SYSTEM() 时启动关卡还没加载完（EditorSceneManager 在自己的
    /// Initialize 里才打开它），当时 BeginPIE 会因为没有活动关卡而失败，
    /// 所以这里只记下请求，等主循环确认关卡就绪后再真正进入播放态。
    void RequestPieOnLevelReady(float seconds) { m_PieSeconds = seconds; }
    /// 让 `--pie` 冒烟测试在播放结束时正常退出播放态，从而覆盖 EndPIE
    /// （Play World 卸载 / 编辑器世界恢复）这条路径。不设的话冒烟测试是
    /// 到点直接退出进程，停止路径永远跑不到。
    void RequestPieStopAtEnd() { m_PieStopRequested = true; }

    /// 让 `--pie` 冒烟测试真正驱动角色移动：`--pie` 的无人值守运行没有键盘，
    /// InputSystem 的命令恒为 0，角色一动不动，跑完"通过"却从未执行过
    /// CharacterController::move()。开启后每帧注入前进命令，并在结束时
    /// 断言角色确实发生了位移。
    void RequestPieMoveTest() { m_PieMoveTest = true; }

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

    /// 进入播放态时建立游戏层（GameInstance / LocalPlayer / PlayerController
    /// + 接管角色）。失败会回滚到编辑态。
    void EnterPIE();
    /// 退出播放态时拆掉游戏层，并重载关卡丢弃 PIE 期间的运行期改动。
    void ExitPIE();

    // Renders one frame at the live window size from inside the Win32 modal resize/move loop
    // (driven by WindowSystem's window-refresh callback while Run() is blocked in glfwPollEvents).
    // UE-style: the parallel pipeline is drained (FlushRenderingCommands) before AND after so the
    // resize frame is fully synchronous and never overlaps the async render/RHI threads -- this is
    // what keeps it from wedging the ImGui game/render handshake the way a naive async re-entry did.
    void RenderFrameDuringResize();

    /// 取当前活动角色的世界位置；无人值守移动测试用它记录起止点。
    bool TryGetActiveCharacterPosition(Vector3& out_position) const;
    /// `--pie-move-test` 结束时断言角色确实移动过，并写日志。
    void ReportPieMoveTestResult();

    std::shared_ptr<EditorUI> m_EditorUi;
    std::string m_ProjectPath;
    EditorPlaybackState m_PlaybackState {EditorPlaybackState::Editing};
    bool m_StepFrameRequested {false};
    // > 0 时表示 `--pie` 请求了一次无人值守播放，但还在等关卡加载完成。
    float m_PieSeconds {0.0f};
    bool m_PieStarted {false};
    /// `--pie-stop`：播放满时长后正常退出播放态（覆盖 EndPIE 路径）。
    bool m_PieStopRequested {false};
    /// `--pie-move-test`：无人值守运行时注入前进命令并断言角色位移。
    bool m_PieMoveTest {false};
    /// 移动测试在开始时记录的角色位置，用于结束时算位移。
    Vector3 m_PieMoveTestStartPosition;
    bool m_PieMoveTestPositionCaptured {false};
    /// 移动测试的终点位置。必须在退出播放态之前取样：EndPIE 一旦丢弃 Play
    /// World，getCurrentActiveLevel() 就变回编辑器世界，那时再读拿到的是
    /// 编辑器里那份从未移动过的角色，位移恒为 0。
    Vector3 m_PieMoveTestEndPosition;
    bool m_PieMoveTestEndCaptured {false};
    /// 移动测试失败时置位，供无人值守脚本判定。
    bool m_PieMoveTestFailed {false};
    /// 判定"确实移动过"的最小位移（米）。远小于一帧的位移量，但足以区分
    /// "真的走了"和"碰撞被挡 / 控制器未配置导致原地不动"。
    static constexpr float kPieMoveTestMinDistance = 0.05f;
    /// 进入播放态的时刻，用于算已播放时长。
    std::chrono::steady_clock::time_point m_PieStartClock;
    // Re-entrancy guard for RenderFrameDuringResize() (the refresh callback can fire recursively
    // when nested message pumps deliver WM_PAINT during the Win32 modal resize/move loop).
    bool m_InResizeRender {false};

#ifdef _WIN32
    HMODULE m_ProjectLibraryHandle {nullptr};
#else
    void* m_ProjectLibraryHandle {nullptr};
#endif
};
