#include "Runtime/Resource/Config/ConfigManager.h"

#include "Runtime/Application/Application.h"
#include "Runtime/Core/Base/CrashHandler.h"
#include "Runtime/Core/Base/ZFileStream.h"
#include "Runtime/Function/Command/CommandSystem.h"

#include <filesystem>
#include <string>

std::vector<std::type_index> ConfigManager::GetDependencies() const
{
    return {GET_SYSTEM_TYPE(CommandSystem)};
}

bool ConfigManager::Initialize()
{
    auto&& config_file_path = GET_SYSTEM(CommandSystem)->getConfigFilePath();
    // read configs
    zcore::ZFileReader config_file(config_file_path.string());
    if (!config_file.isOpen())
    {
        // Without this the failure surfaces as a generic "failed to initialize
        // system 'ConfigManager'" with no hint about which path was tried.
        LOG_ERROR(ZEditor, "ConfigManager: cannot open config file '{}'", config_file_path.string());
        // BqLog 是异步的，而 Initialize() 返回 false 之后引擎是抛异常终止的：
        // 不刷这一下，上面那行（唯一说清楚"到底去哪儿找了 ini"的证据）就随缓冲区
        // 一起没了。双击 .app 启动时没有终端，日志是唯一的线索。
        ZEngineForceFlushLogs();

        // 文件**根本不存在**时用默认值继续，不能让整个引擎起不来。
        //
        // 桌面端 ini 是 CMake install 放到可执行文件旁边的（bin/Debug/ZEditor.ini），
        // 缺了通常意味着安装损坏；但移动端 Player 的 .app bundle 里从来就没有这个
        // 文件 —— 2026-10-30 在 iOS 模拟器上第一次真跑起来就撞在这里：
        //   [E] ConfigManager: cannot open config file '<app>/ZEngine.app/ZEditor.ini'
        //   [F] Failed to initialize system 'ConfigManager', rolling back
        // 于是 ZPlayer_Boot 直接失败、App 白屏退出。没有 ini 的正确语义是"全部用
        // 默认值"，不是"启动失败"。
        //
        // 区分"不存在"与"存在但打不开"：后者（权限、被占用）仍然是真故障，保持致命。
        if (!std::filesystem::exists(config_file_path))
        {
            LOG_WARNING(ZEditor,
                        "ConfigManager: no config file at '{}' -- continuing with defaults",
                        config_file_path.string());
            ZEngineForceFlushLogs();
            return true;
        }
        return false;
    }
    char line_buf[4096];
    while (config_file.getline(line_buf, sizeof(line_buf)))
    {
        std::string config_line(line_buf);
        // Remove trailing newline/carriage return
        while (!config_line.empty() && (config_line.back() == '\n' || config_line.back() == '\r'))
            config_line.pop_back();
        size_t seperate_pos = config_line.find_first_of('=');
        if (seperate_pos > 0 && seperate_pos < (config_line.length() - 1))
        {
            std::string name = config_line.substr(0, seperate_pos);
            std::string value = config_line.substr(seperate_pos + 1, config_line.length() - seperate_pos - 1);
            if (name == "BinaryRootFolder")
            {
                m_RootFolder = config_file_path.parent_path() / value.c_str();
            }
            else if (name == "AssetFolder")
            {
                m_AssetFolder = m_RootFolder / value;
            }
            else if (name == "SchemaFolder")
            {
                m_SchemaFolder = m_RootFolder / value.c_str();
            }
            else if (name == "DefaultWorld")
            {
                m_DefaultWorldUrl = value.c_str();
            }
            else if (name == "BigIconFile")
            {
                m_EditorBigIconPath = m_RootFolder / value.c_str();
            }
            else if (name == "SmallIconFile")
            {
                m_EditorSmallIconPath = m_RootFolder / value.c_str();
            }
            else if (name == "FontFile")
            {
                m_EditorFontPath = m_RootFolder / value.c_str();
            }
            else if (name == "GlobalRenderingRes")
            {
                m_GlobalRenderingResUrl = value.c_str();
            }
            else if (name == "GlobalParticleRes")
            {
                m_GlobalParticleResUrl = value.c_str();
            }
#ifdef ENABLE_PHYSICS_DEBUG_RENDERER
            else if (name == "JoltAssetFolder")
            {
                m_JoltPhysicsAssetFolder = m_RootFolder / value;
            }
#endif
            else if (name == "ResourceFolder")
            {
                m_ResourceFolder = m_RootFolder / value.c_str();
            }
        }
    }
    return true;
}

namespace
{
    // 一次遍历收集需要的几个 key。`BinaryRootFolder` 必须和 value 一起读出来：
    // 它是 `BigIconFile` 的基准目录，而它自己是**相对 ini 所在目录**的
    // （部署时是 `.`）。只扫一遍文件，避免"读两遍、其中一遍读到的是旧值"。
    std::filesystem::path ResolveConfigRelativePath(const std::filesystem::path& config_file_path,
                                                    const std::string&          value_key)
    {
        zcore::ZFileReader config_file(config_file_path.string());
        if (!config_file.isOpen())
        {
            return {};
        }

        std::filesystem::path root_folder = config_file_path.parent_path();
        std::filesystem::path value;
        bool                  have_root = false;
        bool                  have_value = false;

        char line_buf[4096];
        while (config_file.getline(line_buf, sizeof(line_buf)))
        {
            std::string config_line(line_buf);
            while (!config_line.empty() && (config_line.back() == '\n' || config_line.back() == '\r'))
            {
                config_line.pop_back();
            }
            const size_t separate_pos = config_line.find_first_of('=');
            if (separate_pos == std::string::npos || separate_pos == 0 || separate_pos == config_line.length() - 1)
            {
                continue;
            }

            const std::string name  = config_line.substr(0, separate_pos);
            const std::string raw   = config_line.substr(separate_pos + 1);
            if (name == "BinaryRootFolder")
            {
                root_folder = config_file_path.parent_path() / raw;
                have_root   = true;
            }
            else if (name == value_key)
            {
                value      = raw;
                have_value = true;
            }
            if (have_root && have_value)
            {
                break;
            }
        }

        if (!have_value)
        {
            return {};
        }
        return root_folder / value;
    }
}  // namespace

std::filesystem::path ResolveEditorBigIconPath(const std::filesystem::path& config_file_path)
{
    return ResolveConfigRelativePath(config_file_path, "BigIconFile");
}

std::filesystem::path ResolveEditorSmallIconPath(const std::filesystem::path& config_file_path)
{
    return ResolveConfigRelativePath(config_file_path, "SmallIconFile");
}

const std::filesystem::path& ConfigManager::GetRootFolder() const
{
    return m_RootFolder;
}

const std::filesystem::path& ConfigManager::GetAssetFolder() const
{
    return m_AssetFolder;
}

const std::filesystem::path& ConfigManager::GetResourceFolder() const
{
    return m_ResourceFolder;
}

const std::filesystem::path& ConfigManager::GetSchemaFolder() const
{
    return m_SchemaFolder;
}

const std::filesystem::path& ConfigManager::GetEditorBigIconPath() const
{
    return m_EditorBigIconPath;
}

const std::filesystem::path& ConfigManager::GetEditorSmallIconPath() const
{
    return m_EditorSmallIconPath;
}

const std::filesystem::path& ConfigManager::GetEditorFontPath() const
{
    return m_EditorFontPath;
}

const eastl::string& ConfigManager::GetDefaultWorldUrl() const
{
    return m_DefaultWorldUrl;
}

const eastl::string& ConfigManager::GetGlobalRenderingResUrl() const
{
    return m_GlobalRenderingResUrl;
}

const eastl::string& ConfigManager::GetGlobalParticleResUrl() const
{
    return m_GlobalParticleResUrl;
}

#ifdef ENABLE_PHYSICS_DEBUG_RENDERER
const std::filesystem::path& ConfigManager::GetJoltPhysicsAssetFolder() const
{
    return m_JoltPhysicsAssetFolder;
}
#endif