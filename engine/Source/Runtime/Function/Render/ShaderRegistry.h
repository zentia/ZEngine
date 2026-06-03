#pragma once

#include "Runtime/Core/Base/EngineSystem.h"

#include <EASTL/unordered_map.h>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <typeindex>
#include <vector>

/**
 * Per-entry view of one user-authored `.shader` under <Project>/Shaders/.
 * Mirrors ScriptAsset / ScriptRegistry (AGENTS.md 2.1): identity lives in
 * shader_registry.json, not in a .meta sidecar.
 */
struct ShaderRegistryEntry
{
    // 32-char lowercase hex (deterministic from project-relative source path).
    eastl::string m_Guid;

    // e.g. "Shaders/PBR/Standard.shader" (forward slashes, lower on Windows).
    eastl::string m_SourceRelPath;

    // ShaderLab `Shader "Name"` or filename stem. Used to resolve materials
    // that store a shader by name string.
    eastl::string m_ShaderName;

    // e.g. "Assets/_Generated/Shaders/PBR/Standard.shader.zasset".
    eastl::string m_ZassetRelPath;

    int64_t m_SourceMtimeNs = 0;

    // FNV-1a hex of full source text; rename detection across editor restarts.
    eastl::string m_SourceContentHash;
};

/**
 * Centralised path<->GUID registry for ShaderLab `.shader` sources.
 * Checked into VCS at <Project>/AssetRegistry/shader_registry.json.
 */
class ShaderRegistry : public IEngineSystem
{
public:
    std::string GetName() const override { return "ShaderRegistry"; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Resource; }
    std::vector<std::type_index> GetDependencies() const override;
    bool Initialize() override;
    void Shutdown() override;

    ShaderRegistryEntry* FindByGuid(const eastl::string& guid);
    const ShaderRegistryEntry* FindByGuid(const eastl::string& guid) const;
    ShaderRegistryEntry* FindByPath(const eastl::string& source_rel_path);
    const ShaderRegistryEntry* FindByPath(const eastl::string& source_rel_path) const;
    ShaderRegistryEntry* FindByName(const eastl::string& shader_name);
    const ShaderRegistryEntry* FindByName(const eastl::string& shader_name) const;

    std::vector<ShaderRegistryEntry*> GetAll() const;

    void rescan();

    /// Editor shader watcher hook (PR-AI2). Call after debounce when a
    /// `.shader` under Shaders/ changed or was removed.
    void OnShaderFileEvent(const std::filesystem::path& abs_shader_path);

    std::filesystem::path GetRegistryFilePath() const;

    /// Editor tick: flush debounced JSON when the quiet window elapsed.
    void TickDeferredSave();

    /// Deterministic GUID from normalised project-relative path (public for tests).
    static eastl::string DeterministicGuidFromPath(const eastl::string& rel_path);

    /// Project-relative path of the generated ShaderRes .zasset for a source
    /// file relative to Shaders/ (e.g. "PBR/Foo.shader" -> "Assets/...zasset").
    static eastl::string ComputeZassetRelPath(const eastl::string& rel_under_shaders_root);

private:
    bool LoadFromDisk();
    bool SaveToDisk() const;
    void ScheduleSave();
    void FlushPendingSave() const;

    static std::vector<eastl::string> EnumerateShaderFiles(const std::filesystem::path& project_root,
                                                           const std::filesystem::path& shaders_root);
    static eastl::string ParseShaderNameFromSource(const std::filesystem::path& abs_path);
    static int64_t FileMTimeNs(const std::filesystem::path& abs_path);
    static eastl::string NormaliseRelPath(const std::filesystem::path& rel_path);
    static eastl::string NormaliseShaderNameKey(const eastl::string& name);

    void UpsertEntryForAbsPath(const std::filesystem::path& abs_shader_path, bool* out_changed);
    void RemoveEntryForAbsPath(const std::filesystem::path& abs_shader_path, bool* out_changed);
    void RebuildNameIndexForEntry(const ShaderRegistryEntry& entry);
    void RemoveNameIndexForEntry(const ShaderRegistryEntry& entry);

    eastl::unordered_map<eastl::string, ShaderRegistryEntry> m_ByGuid;
    eastl::unordered_map<eastl::string, eastl::string> m_GuidByPath;
    eastl::unordered_map<eastl::string, eastl::string> m_GuidByName;
    mutable std::mutex m_Mutex;
    mutable bool m_SavePending {false};
    mutable std::chrono::steady_clock::time_point m_SaveDeadline {};

    std::filesystem::path m_ProjectRoot;
    std::filesystem::path m_ShadersRoot;
    std::filesystem::path m_RegistryFile;
};
