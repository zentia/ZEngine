#include "EditorScriptingManager.h"

#include "Editor/Scripting/EditorScriptHooks.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Config/ConfigManager.h"
#include "Runtime/Scripting/Binding/ScriptBinding.h"
#include "Runtime/Scripting/ScriptingManager.h"
#include "Runtime/Scripting/Wrapper/ScriptEnv.h"
#include "core/Log/LogSystem.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

namespace
{
    constexpr const char* kBootstrapModule = "Bootstrap";
    constexpr const char* kOnLoadFunction = "OnEditorLoad";
    constexpr const char* kOnUnloadFunction = "OnEditorUnload";

    bool ReadAll(const std::filesystem::path& path, std::string& out)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }

    // Only touch the file when the content changed, so tsc --watch / IDE file
    // watchers do not see a rewrite on every editor start.
    void WriteIfChanged(const std::filesystem::path& path, const std::string& content)
    {
        std::string existing;
        if (ReadAll(path, existing) && existing == content)
            return;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            LOG_WARNING(ZScripting, "EditorScriptingManager: cannot write typings file: {}", path.generic_string());
            return;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    // <repo>/engine/EditorScripts, derived from this file's own location so a
    // source checkout needs no build-time define. In a shipped build the path
    // simply does not exist on the user's disk and the lookup yields empty.
    std::filesystem::path SourceCheckoutScriptsDir()
    {
        std::filesystem::path here = std::filesystem::path(__FILE__).lexically_normal();
        // <engine>/Source/Editor/Scripting/EditorScriptingManager.cpp
        std::filesystem::path engine_dir = here.parent_path().parent_path().parent_path().parent_path();
        std::filesystem::path candidate = engine_dir / "EditorScripts";
        std::error_code ec;
        return std::filesystem::is_directory(candidate, ec) ? candidate : std::filesystem::path {};
    }
}  // namespace

std::vector<std::type_index> EditorScriptingManager::GetDependencies() const
{
    // ScriptingManager first: it performs the one-time pesapi registry setup
    // (ScriptBindingRegistry::InstallAll) that every VM shares.
    return {GET_SYSTEM_TYPE(ScriptingManager), GET_SYSTEM_TYPE(ProjectInfo), GET_SYSTEM_TYPE(ConfigManager)};
}

std::filesystem::path EditorScriptingManager::ResolveScriptsRoot() const
{
    std::error_code ec;
    if (const char* override_dir = std::getenv("ZENGINE_EDITOR_SCRIPTS_DIR"))
    {
        if (override_dir[0] != '\0')
            return std::filesystem::path(override_dir);
    }
    if (auto config = GET_SYSTEM(ConfigManager))
    {
        std::filesystem::path deployed = config->GetRootFolder() / "EditorScripts";
        if (std::filesystem::is_directory(deployed, ec))
            return deployed.lexically_normal();
    }
    return {};
}

void EditorScriptingManager::WriteTypings() const
{
    const std::filesystem::path source_dir = SourceCheckoutScriptsDir();
    if (source_dir.empty())
        return;
    const std::filesystem::path typings = source_dir / "typings";
    WriteIfChanged(typings / "zengine.d.ts", ScriptBindingRegistry::BuildTypeScriptDeclarationFile());
    WriteIfChanged(typings / "zengine-editor.d.ts", ScriptBindingRegistry::BuildEditorTypeScriptDeclarationFile());
}

bool EditorScriptingManager::Initialize()
{
    // Keep the typings fresh even when the compiled scripts are missing: that
    // is exactly when an engine developer needs them to run the first tsc.
    WriteTypings();

    m_ScriptsRoot = ResolveScriptsRoot();
    if (m_ScriptsRoot.empty())
    {
        LOG_WARNING(ZScripting,
                    "EditorScriptingManager: no EditorScripts directory (build the ZEditorScripts target or set "
                    "ZENGINE_EDITOR_SCRIPTS_DIR); editor script layer disabled.");
        return true;
    }

    int debug_port = -1;
    if (const char* env_port = std::getenv("ZENGINE_EDITOR_V8_DEBUG_PORT"))
    {
        char* end = nullptr;
        const long v = std::strtol(env_port, &end, 10);
        if (end != env_port && v > 0 && v < 65536)
            debug_port = static_cast<int>(v);
    }

    m_Env = ScriptingManager::CreateIsolatedEnv(/*editorDomain=*/true, debug_port);
    if (m_Env == nullptr)
    {
        LOG_WARNING(ZScripting, "EditorScriptingManager: this build has no JS backend; editor script layer disabled.");
        m_ScriptsRoot.clear();
        return true;
    }

    m_Env->SetJsRoot(m_ScriptsRoot.generic_string());
    LOG_INFO(ZScripting, "EditorScriptingManager: editor VM ready, scripts root: {}", m_ScriptsRoot.generic_string());

    if (!m_Env->LoadModule(kBootstrapModule))
    {
        LOG_ERROR(ZScripting, "EditorScriptingManager: failed to load '{}.js' from {}", kBootstrapModule,
                  m_ScriptsRoot.generic_string());
        return true;  // a broken editor script must not stop the editor from booting
    }
    m_Bootstrapped = m_Env->InvokeExportedFunction(kBootstrapModule, kOnLoadFunction);
    return true;
}

void EditorScriptingManager::Tick()
{
    if (m_Env != nullptr)
        m_Env->Tick();
}

void EditorScriptingManager::Shutdown()
{
    if (m_Env == nullptr)
        return;
    if (m_Bootstrapped)
        m_Env->InvokeExportedFunction(kBootstrapModule, kOnUnloadFunction);
    delete m_Env;
    m_Env = nullptr;
    m_Bootstrapped = false;
    // P2: drop menu items / Inspector buttons with the VM that owns their
    // handlers. Leaving them registered would show rows that call into a
    // destroyed env -- Invoke() would refuse, but the UI would be lying about
    // what the editor can do.
    EditorScriptHooks::Clear();
}
