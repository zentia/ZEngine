#pragma once

#include "Runtime/Core/Base/EngineSystem.h"

#include <filesystem>
#include <string>
#include <typeindex>
#include <vector>

class ScriptEnv;

/**
 * @brief Owns the editor-scripting VM -- the C# half of "native core + managed
 *        editor layer" that Unity gets from UnityEditor.dll.
 *
 * Two VMs, one registry
 * ---------------------
 * ScriptingManager owns the *game* VM: it only ever sees ZScriptDomain::Runtime
 * bindings, so a user script compiled against zengine.d.ts cannot reach editor
 * API at runtime either. This system creates a second, isolated VM whose
 * globals additionally include the ZScriptDomain::Editor bindings (classes
 * annotated in the ZEditor module, e.g. `EditorEnvironment`).
 *
 * Where the editor scripts live
 * -----------------------------
 * Engine-shipped editor scripts are precompiled JavaScript under
 * `<BinaryRoot>/EditorScripts/` (built from `engine/EditorScripts/src/*.ts` by
 * the `ZEditorScripts` CMake target). They are an engine asset, not a project
 * asset: they do not depend on tsc being installed on the user's machine and
 * are not hot-reloaded. Override the directory with the environment variable
 * `ZENGINE_EDITOR_SCRIPTS_DIR` (engine developers pointing at a tsc --watch
 * output folder).
 *
 * Bootstrap contract
 * ------------------
 * `Bootstrap.js` must export `OnEditorLoad()` (called once after the VM is up)
 * and `OnEditorUnload()` (called before the VM is torn down, only if load
 * succeeded).
 *
 * Typings
 * -------
 * In a source checkout (ZENGINE_EDITOR_SCRIPTS_SOURCE_DIR is defined and
 * exists) the editor rewrites `<source>/typings/zengine.d.ts` and
 * `zengine-editor.d.ts` on every start, so IDE IntelliSense and the
 * ZEditorScripts tsc step always see the bindings the editor really has.
 *
 * Lifecycle
 * ---------
 * PostInit, after ScriptingManager (shared pesapi registry / backend init) and
 * ProjectInfo. A missing scripts directory is not an error -- the editor
 * simply runs without its script layer.
 */
class EditorScriptingManager : public IEngineSystem
{
public:
    EditorScriptingManager() = default;
    ~EditorScriptingManager() override = default;

    std::string GetName() const override { return "EditorScriptingManager"; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PostInit; }
    std::vector<std::type_index> GetDependencies() const override;
    bool Initialize() override;
    void Shutdown() override;

    /// Per-frame backend tick (microtasks / timers). Cheap when idle.
    void Tick();

    bool IsRunning() const { return m_Env != nullptr; }

    /// Absolute directory the editor VM resolves modules against; empty when
    /// the script layer is not running.
    const std::filesystem::path& GetScriptsRoot() const { return m_ScriptsRoot; }

    /// The editor VM, or nullptr when it is not running. Main thread only.
    ScriptEnv* GetEnv() { return m_Env; }

private:
    std::filesystem::path ResolveScriptsRoot() const;
    void WriteTypings() const;

    ScriptEnv* m_Env = nullptr;
    std::filesystem::path m_ScriptsRoot;
    bool m_Bootstrapped = false;
};
