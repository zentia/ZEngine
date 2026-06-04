#include "Runtime/Function/Render/LargeWorldCoordinatesSettings.h"

#include "Runtime/Core/Math/LargeWorldCoordinates.h"
#include "Runtime/Function/Console/ConsoleManager.h"

namespace LargeWorldCoordinatesSettings
{
namespace
{
    bool g_lwc_enable_storage {false};
}

void RegisterConsoleVariables(ConsoleManager& console)
{
    g_lwc_enable_storage = LargeWorldCoordinates::IsEnabled();

    auto cvar = console.RegisterBoolVariable(
        "r.LWC.Enable",
        "Large World Coordinates: rebase render math to 2^21 world-unit tiles (UE-style).",
        g_lwc_enable_storage,
        &g_lwc_enable_storage);
    if (cvar)
    {
        cvar->setOnChangedCallback([]() { LargeWorldCoordinates::SetEnabled(g_lwc_enable_storage); });
    }
}

}  // namespace LargeWorldCoordinatesSettings
