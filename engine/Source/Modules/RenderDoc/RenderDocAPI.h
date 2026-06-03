#pragma once

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)

#include "renderdoc_app.h"

namespace Z
{
    struct RenderDocInitParams
    {
        const char* dll_path_override = nullptr;
        bool load_module = false;
    };

    class RenderDocAPI
    {
    public:
        // Resolve install path. Optionally load the module when load_module is true.
        static void Init(const RenderDocInitParams& params = {});

        static bool IsInstalled();
        static void LoadModule();

        // Null when RenderDoc is not loaded.
        static RENDERDOC_API_1_0_0* Get();

        static const char* GetModulePath();
    };
}  // namespace Z

#endif
