#include "Runtime/Resource/Config/ConfigManager.h"

#include "Runtime/Application/Application.h"
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
        return false;
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