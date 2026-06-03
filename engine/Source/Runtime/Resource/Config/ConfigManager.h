#pragma once

#include "Runtime/Core/Base/EngineSystem.h"

#include <filesystem>

class ConfigManager : public IEngineSystem
{
public:
    std::string GetName() const override { return "ConfigManager"; }
    std::vector<std::type_index> GetDependencies() const override;
    bool Initialize() override;
    void Shutdown() override {}
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PreInit; }

    const std::filesystem::path& GetRootFolder() const;
    const std::filesystem::path& GetAssetFolder() const;
    const std::filesystem::path& GetResourceFolder() const;
    const std::filesystem::path& GetSchemaFolder() const;
    const std::filesystem::path& GetEditorBigIconPath() const;
    const std::filesystem::path& GetEditorSmallIconPath() const;
    const std::filesystem::path& GetEditorFontPath() const;

#ifdef ENABLE_PHYSICS_DEBUG_RENDERER
    const std::filesystem::path& GetJoltPhysicsAssetFolder() const;
#endif

    const eastl::string& GetDefaultWorldUrl() const;
    const eastl::string& GetGlobalRenderingResUrl() const;
    const eastl::string& GetGlobalParticleResUrl() const;

private:
    std::filesystem::path m_RootFolder;
    std::filesystem::path m_ResourceFolder;
    std::filesystem::path m_AssetFolder;
    std::filesystem::path m_SchemaFolder;
    std::filesystem::path m_EditorBigIconPath;
    std::filesystem::path m_EditorSmallIconPath;
    std::filesystem::path m_EditorFontPath;

#ifdef ENABLE_PHYSICS_DEBUG_RENDERER
    std::filesystem::path m_JoltPhysicsAssetFolder;
#endif

    eastl::string m_DefaultWorldUrl;
    eastl::string m_GlobalRenderingResUrl;
    eastl::string m_GlobalParticleResUrl;
};