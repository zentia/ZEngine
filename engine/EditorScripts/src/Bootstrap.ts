// Entry point of the engine's TypeScript editor layer.
//
// EditorScriptingManager loads this module in the editor VM right after
// startup and calls OnEditorLoad(); OnEditorUnload() runs before the VM is
// destroyed. Everything declared in typings/zengine-editor.d.ts (e.g.
// EditorEnvironment) exists only in this VM -- game scripts cannot see it.

export function OnEditorLoad(): void {
    console.log(
        "[EditorScripts] Bootstrap loaded. project='" + EditorEnvironment.GetProjectRoot() +
        "' playing=" + EditorEnvironment.IsPlaying());
}

export function OnEditorUnload(): void {
    console.log("[EditorScripts] Bootstrap unloading.");
}
