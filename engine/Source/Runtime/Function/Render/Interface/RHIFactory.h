#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Function/Render/RenderType.h"

#include <memory>
#include <typeindex>

class RHI;
class WindowSystem;

// RHI Factory for creating RHI instances based on GraphicsAPI
class RHIFactory
{
public:
    // Create RHI instance based on API type
    static RHI* CreateRHI(GraphicsAPI api, WindowSystem* window_system);

    // Get default API based on platform
    static GraphicsAPI GetDefaultAPI();

    // Check if API is available on current platform
    static bool IsAPIAvailable(GraphicsAPI api);
};