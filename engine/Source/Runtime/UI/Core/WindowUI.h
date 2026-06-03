#pragma once

#include <memory>

class WindowSystem;
class RenderSystem;

struct WindowUIInitInfo
{
    WindowSystem* window_system;
    RenderSystem* render_system;
};

class WindowUI
{
public:
    virtual ~WindowUI() = default;
    virtual bool Initialize() = 0;
    virtual void PreRender() = 0;
};