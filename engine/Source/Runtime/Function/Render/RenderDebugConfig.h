#pragma once

#include "Runtime/Core/Base/EngineSystem.h"

class RenderDebugConfig : public IEngineSystem
{
public:
    std::string GetName() const override { return "RenderDebugConfig"; }
    bool Initialize() override { return true; }
    void Shutdown() override {}
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Rendering; }
    struct Animation
    {
        bool show_skeleton = false;
        bool show_bone_name = false;
    };
    struct Camera
    {
        bool show_runtime_info = false;
    };
    struct GameObject
    {
        bool show_bounding_box = false;
    };

    Animation animation;
    Camera camera;
    GameObject game_object;
};