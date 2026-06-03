#include "RenderDocAPI.h"

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)

#include "Runtime/Core/Base/Macro.h"

#include <Windows.h>

#include <filesystem>
#include <string>

namespace
{
    RENDERDOC_API_1_0_0* g_renderdoc_api = nullptr;
    std::string g_renderdoc_module_path;

    std::string WideToUtf8(const wchar_t* wide)
    {
        if (wide == nullptr || wide[0] == L'\0')
        {
            return {};
        }

        const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
        if (size <= 1)
        {
            return {};
        }

        std::string utf8(static_cast<size_t>(size - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), size, nullptr, nullptr);
        return utf8;
    }

    std::wstring Utf8ToWide(const std::string& utf8)
    {
        if (utf8.empty())
        {
            return {};
        }

        const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        if (size <= 1)
        {
            return {};
        }

        std::wstring wide(static_cast<size_t>(size - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), size);
        return wide;
    }

    HMODULE FindLoadedRenderDocModule()
    {
        return GetModuleHandleW(L"renderdoc.dll");
    }

    void InitRenderDocApi(HMODULE module)
    {
        if (module == nullptr)
        {
            return;
        }

        auto* get_api = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
        if (get_api == nullptr)
        {
            LOG_WARNING(ZRender, "RenderDocAPI: RENDERDOC_GetAPI export not found");
            return;
        }

        RENDERDOC_API_1_0_0* api = nullptr;
        const int success = get_api(eRENDERDOC_API_Version_1_0_0, reinterpret_cast<void**>(&api));
        if (!success || api == nullptr)
        {
            LOG_WARNING(ZRender, "RenderDocAPI: RENDERDOC_GetAPI failed");
            return;
        }

        g_renderdoc_api = api;
        g_renderdoc_api->MaskOverlayBits(0, 0);
        g_renderdoc_api->SetFocusToggleKeys(nullptr, 0);
        g_renderdoc_api->SetCaptureKeys(nullptr, 0);
        g_renderdoc_api->UnloadCrashHandler();
        LOG_INFO(ZRender, "RenderDocAPI: loaded from {}", g_renderdoc_module_path);
    }

    std::filesystem::path ResolveEngineRepoRoot()
    {
        if (const char* env_root = std::getenv("ZENGINE_ROOT"))
        {
            if (env_root[0] != '\0')
            {
                return std::filesystem::path(env_root);
            }
        }

#ifdef ZENGINE_REPO_ROOT
        return std::filesystem::path(ZENGINE_REPO_ROOT);
#else
        return {};
#endif
    }

    std::string ResolveBundledRenderDocDll()
    {
        const std::filesystem::path repo_root = ResolveEngineRepoRoot();
        if (repo_root.empty())
        {
            return {};
        }

        const std::filesystem::path bundled =
            repo_root / "tools" / "renderdoc" / "x64" / "Development" / "renderdoc.dll";
        if (std::filesystem::exists(bundled))
        {
            return bundled.string();
        }
        return {};
    }

    std::string ResolveRegistryRenderDocDll()
    {
        HKEY key = nullptr;
        constexpr const wchar_t* k_clsid_path =
            L"Software\\Classes\\CLSID\\{5D6BF029-A6BA-417A-8523-120492B1DCE3}\\InprocServer32";
        if (RegOpenKeyExW(HKEY_CURRENT_USER, k_clsid_path, 0, KEY_READ, &key) != ERROR_SUCCESS)
        {
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, k_clsid_path, 0, KEY_READ, &key) != ERROR_SUCCESS)
            {
                return {};
            }
        }

        wchar_t value[MAX_PATH] = {};
        DWORD value_size = sizeof(value);
        DWORD type = 0;
        std::string result;
        if (RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<LPBYTE>(value), &value_size) ==
                ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ))
        {
            result = WideToUtf8(value);
        }
        RegCloseKey(key);
        return result;
    }

    std::string ResolveRenderDocModulePath(const Z::RenderDocInitParams& params)
    {
        if (params.dll_path_override != nullptr && params.dll_path_override[0] != '\0')
        {
            return params.dll_path_override;
        }

        if (const char* env_path = std::getenv("ZENGINE_RENDERDOC_DLL"))
        {
            if (env_path[0] != '\0')
            {
                return env_path;
            }
        }

        const std::string bundled = ResolveBundledRenderDocDll();
        if (!bundled.empty())
        {
            return bundled;
        }

        return ResolveRegistryRenderDocDll();
    }

    HMODULE LoadRenderDocDynamicLibrary()
    {
        if (g_renderdoc_module_path.empty())
        {
            return nullptr;
        }

        const std::wstring wide_path = Utf8ToWide(g_renderdoc_module_path);
        if (wide_path.empty())
        {
            return nullptr;
        }

        return LoadLibraryW(wide_path.c_str());
    }
}  // namespace

namespace Z
{
    void RenderDocAPI::Init(const RenderDocInitParams& params)
    {
        g_renderdoc_module_path = ResolveRenderDocModulePath(params);
        if (g_renderdoc_module_path.empty())
        {
            LOG_INFO(ZRender,
                     "RenderDocAPI: no RenderDoc install found (use -load-renderdoc, --renderdoc-dll, or build "
                     "tools/renderdoc)");
            return;
        }

        HMODULE module = nullptr;
        if (params.load_module)
        {
            module = LoadRenderDocDynamicLibrary();
        }

        if (module == nullptr)
        {
            module = FindLoadedRenderDocModule();
        }

        if (module != nullptr)
        {
            InitRenderDocApi(module);
        }
    }

    bool RenderDocAPI::IsInstalled()
    {
        return !g_renderdoc_module_path.empty();
    }

    void RenderDocAPI::LoadModule()
    {
        if (g_renderdoc_api != nullptr)
        {
            return;
        }

        HMODULE module = LoadRenderDocDynamicLibrary();
        if (module == nullptr)
        {
            module = FindLoadedRenderDocModule();
        }

        if (module != nullptr)
        {
            InitRenderDocApi(module);
        }
        else
        {
            LOG_ERROR(ZRender, "RenderDocAPI: failed to load {}", g_renderdoc_module_path);
        }
    }

    RENDERDOC_API_1_0_0* RenderDocAPI::Get()
    {
        return g_renderdoc_api;
    }

    const char* RenderDocAPI::GetModulePath()
    {
        return g_renderdoc_module_path.c_str();
    }
}  // namespace Z

#else

namespace Z
{
    void RenderDocAPI::Init(const RenderDocInitParams&) {}
    bool RenderDocAPI::IsInstalled() { return false; }
    void RenderDocAPI::LoadModule() {}
    RENDERDOC_API_1_0_0* RenderDocAPI::Get() { return nullptr; }
    const char* RenderDocAPI::GetModulePath() { return ""; }
}  // namespace Z

#endif
