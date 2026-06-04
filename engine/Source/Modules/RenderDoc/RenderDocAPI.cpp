#include "RenderDocAPI.h"

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)

#include "Runtime/Core/Base/Macro.h"

#include <Windows.h>
#include <ShlObj.h>

#include <filesystem>
#include <fstream>
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

    std::filesystem::path GetExecutableDirectory()
    {
        wchar_t exe_path[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) == 0)
        {
            return {};
        }
        return std::filesystem::path(exe_path).parent_path();
    }

    std::filesystem::path NormalizeRenderDocPath(std::filesystem::path path)
    {
        if (path.empty())
        {
            return {};
        }

        if (path.is_relative())
        {
            const std::filesystem::path exe_dir = GetExecutableDirectory();
            if (!exe_dir.empty())
            {
                path = exe_dir / path;
            }
            else
            {
                path = std::filesystem::absolute(path);
            }
        }

        std::error_code ec;
        const std::filesystem::path normalized = std::filesystem::weakly_canonical(path, ec);
        return ec ? path.lexically_normal() : normalized;
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
#ifdef ZENGINE_REPO_ROOT
        const std::filesystem::path compile_root = std::filesystem::path(ZENGINE_REPO_ROOT);
        if (std::filesystem::exists(compile_root / "tools" / "renderdoc" / "renderdoc.sln"))
        {
            return compile_root;
        }
#endif

        const std::filesystem::path exe_dir = GetExecutableDirectory();
        if (!exe_dir.empty())
        {
            // ZEditor.exe lives at <repo>/bin/<Config>/ZEditor.exe
            const std::filesystem::path bin_dir = exe_dir.parent_path().parent_path();
            if (bin_dir.filename() == "bin")
            {
                const std::filesystem::path repo_root = bin_dir.parent_path();
                if (std::filesystem::exists(repo_root / "tools" / "renderdoc" / "renderdoc.sln"))
                {
                    return repo_root;
                }
            }
        }

        if (const char* env_root = std::getenv("ZENGINE_ROOT"))
        {
            if (env_root[0] != '\0')
            {
                const std::filesystem::path env_path(env_root);
                if (std::filesystem::exists(env_path / "tools" / "renderdoc" / "renderdoc.sln"))
                {
                    return env_path;
                }
            }
        }

#ifdef ZENGINE_REPO_ROOT
        return compile_root;
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
            return NormalizeRenderDocPath(bundled).string();
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

    std::string ResolveExeAdjacentRenderDocDll()
    {
        const std::filesystem::path exe_dir = GetExecutableDirectory();
        if (exe_dir.empty())
        {
            return {};
        }

        const std::filesystem::path dll = exe_dir / "renderdoc.dll";
        if (std::filesystem::exists(dll))
        {
            return NormalizeRenderDocPath(dll).string();
        }
        return {};
    }

    std::string NormalizeRenderDocModulePathString(const std::string& path_utf8)
    {
        if (path_utf8.empty())
        {
            return {};
        }

        const std::filesystem::path normalized = NormalizeRenderDocPath(std::filesystem::path(path_utf8));
        if (normalized.empty() || !std::filesystem::exists(normalized))
        {
            return {};
        }
        return normalized.string();
    }

    std::string ResolveRenderDocModulePath(const Z::RenderDocInitParams& params)
    {
        if (params.dll_path_override != nullptr && params.dll_path_override[0] != '\0')
        {
            const std::string resolved = NormalizeRenderDocModulePathString(params.dll_path_override);
            if (!resolved.empty())
            {
                return resolved;
            }
        }

        if (const char* env_path = std::getenv("ZENGINE_RENDERDOC_DLL"))
        {
            if (env_path[0] != '\0')
            {
                const std::string resolved = NormalizeRenderDocModulePathString(env_path);
                if (!resolved.empty())
                {
                    return resolved;
                }
            }
        }

        const std::string adjacent = ResolveExeAdjacentRenderDocDll();
        if (!adjacent.empty())
        {
            return adjacent;
        }

        const std::string bundled = ResolveBundledRenderDocDll();
        if (!bundled.empty())
        {
            return bundled;
        }

        const std::string registry = ResolveRegistryRenderDocDll();
        return NormalizeRenderDocModulePathString(registry);
    }

    bool WriteBinaryFileIfMissing(const std::filesystem::path& file,
                                  const void* data,
                                  size_t size,
                                  size_t legacy_replace_size = 0)
    {
        if (std::filesystem::exists(file))
        {
            std::error_code ec;
            const auto existing_size = std::filesystem::file_size(file, ec);
            if (ec)
            {
                return false;
            }

            if (existing_size != 0 && existing_size != legacy_replace_size)
            {
                return false;
            }
        }

        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            LOG_WARNING(ZRender, "RenderDocAPI: failed to seed {}", file.string());
            return false;
        }

        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!out)
        {
            LOG_WARNING(ZRender, "RenderDocAPI: failed to write {}", file.string());
            return false;
        }

        LOG_INFO(ZRender, "RenderDocAPI: seeded {}", file.string());
        return true;
    }

    void EnsureRenderDocAppData()
    {
        // RenderDoc opens several files under %APPDATA%/renderdoc/ during hook init.
        // Missing files -> StreamReader(NULL) or EOF reads -> RDCERR -> __debugbreak in
        // Development builds. Seed minimal valid placeholders before LoadLibrary.
        std::filesystem::path appdata;
        if (const wchar_t* env = _wgetenv(L"APPDATA"))
        {
            appdata = env;
        }
        else
        {
            wchar_t path_buffer[MAX_PATH] = {};
            if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, path_buffer)))
            {
                return;
            }
            appdata = path_buffer;
        }

        const std::filesystem::path app_dir = appdata / "renderdoc";
        std::error_code ec;
        std::filesystem::create_directories(app_dir, ec);

        static constexpr const char k_min_conf[] =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
            "<config version=\"1\">\r\n"
            "</config>\r\n";
        WriteBinaryFileIfMissing(app_dir / "renderdoc.conf",
                                 k_min_conf,
                                 sizeof(k_min_conf) - 1);

        // Empty D3D11/D3D12 shader cache (magic RD$$, local 0xf000baba, version 3, 0 entries).
        // Compressed payload uses RenderDoc's uint32 size prefix + zstd chunk (not a raw zstd frame).
        static const uint8_t k_empty_d3d_shader_cache[] = {
            0x52, 0x44, 0x24, 0x24, 0xba, 0xba, 0x00, 0xf0, 0x03, 0x00, 0x00, 0x00,
            0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00,
            0x28, 0xb5, 0x2f, 0xfd, 0x20, 0x04, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        WriteBinaryFileIfMissing(app_dir / "d3dshaders.cache",
                                 k_empty_d3d_shader_cache,
                                 sizeof(k_empty_d3d_shader_cache),
                                 33);

        // Empty Vulkan shader cache (local magic 0xf00d00d5, version 1, 0 entries).
        static const uint8_t k_empty_vk_shader_cache[] = {
            0x52, 0x44, 0x24, 0x24, 0xd5, 0x00, 0x0d, 0xf0, 0x01, 0x00, 0x00, 0x00,
            0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00,
            0x28, 0xb5, 0x2f, 0xfd, 0x20, 0x04, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        WriteBinaryFileIfMissing(app_dir / "vkshaders.cache",
                                 k_empty_vk_shader_cache,
                                 sizeof(k_empty_vk_shader_cache),
                                 33);
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

        EnsureRenderDocAppData();

        // LOAD_WITH_ALTERED_SEARCH_PATH: resolve renderdoc.dll dependencies from its own
        // directory, not the process CWD (CommandSystem sets CWD to the project folder).
        HMODULE module = LoadLibraryExW(wide_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module == nullptr)
        {
            LOG_WARNING(ZRender,
                        "RenderDocAPI: LoadLibraryEx failed for {} (GetLastError={})",
                        g_renderdoc_module_path,
                        static_cast<unsigned long>(GetLastError()));
        }
        return module;
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
                     "RenderDocAPI: no RenderDoc install found (use --renderdoc-dll or build tools/renderdoc)");
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
        else if (params.load_module)
        {
            LOG_INFO(ZRender,
                     "RenderDocAPI: module path resolved to {} but load failed (cwd={})",
                     g_renderdoc_module_path,
                     std::filesystem::current_path().string());
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
