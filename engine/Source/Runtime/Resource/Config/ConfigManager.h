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

// -----------------------------------------------------------------------------
// 不依赖任何引擎系统的 ini 读取：给 START_SYSTEM() **之前** 的代码用。
//
// 唯一的消费者是 ZEditor Launcher —— 它在 NSApp 起来那一刻就要有 Dock 图标
// （ActivationPolicyRegular 一设，Dock 立刻出现磁贴；那时候 ConfigManager 还没
// 初始化，SystemRegistry 里根本没它）。
//
// 放在这里而不是 Launcher 里重写一遍解析，是因为"BigIconFile 怎么变成绝对路径"
// 这条规则（= BinaryRootFolder 相对 ini 所在目录）将来改 bundle 布局时一定会变，
// 两份实现必然各自漂移。
//
// 找不到 / 打不开就返回空路径。
// -----------------------------------------------------------------------------
std::filesystem::path ResolveEditorBigIconPath(const std::filesystem::path& config_file_path);
std::filesystem::path ResolveEditorSmallIconPath(const std::filesystem::path& config_file_path);