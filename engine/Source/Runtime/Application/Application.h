#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Log/LogSystem.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

extern bool g_isPlaying;
extern std::unordered_set<std::string> g_editorTickComponentTypes;

class Application : public Object, public IEngineSystem
{
    friend class Editor;

    static const float s_Fpsalpha;

public:
    std::string GetName() const override { return GET_CLASS_NAME(Application); }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Core; }
    std::vector<std::type_index> GetDependencies() const override;
    bool Initialize() override;
    void Shutdown() override;

    bool IsQuit() const { return m_Isquit; }
    void RequestQuit() { m_Isquit = true; }
    void Run();
    bool TickOneFrame(float deltaTime);
    bool TickOneFrame(float simulationDeltaTime, float frameDeltaTime, bool shouldUpdateLogic);

    int GetFps() const { return m_Fps; }
    bool MayUpdate() const { return m_DisabllowUpdating == 0; }

protected:
    void LogicalTick(float deltaTime);
    bool RendererTick(float delta_time);

    void CalculateFPS(float delta_time);

    /**
     *  Each frame can only be called once
     */
    float CalculateDeltaTime();

    bool m_Isquit {false};

    std::chrono::steady_clock::time_point m_LastTickTimePoint {std::chrono::steady_clock::now()};

    float m_AverageDuration {0.f};
    int m_FrameCount {0};
    int m_Fps {0};
    int m_DisabllowUpdating {0};
};
