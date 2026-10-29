// Entry point of the engine's TypeScript editor layer.
//
// EditorScriptingManager loads this module in the editor VM right after
// startup and calls OnEditorLoad(); OnEditorUnload() runs before the VM is
// destroyed. Everything declared in typings/zengine-editor.d.ts (e.g.
// EditorEnvironment) exists only in this VM -- game scripts cannot see it.

function reportEnvironment(): void {
    console.log(
        "[EditorScripts] Bootstrap loaded. project='" + EditorEnvironment.GetProjectRoot() +
        "' playing=" + EditorEnvironment.IsPlaying());
}

// P1 self-check: calls every facade once, so the acceptance run proves the
// bindings are reachable AND return real data -- not merely that the module
// loaded. Read-only on purpose: Bootstrap runs on every editor start, so it
// must never mutate the user's selection or undo history.
function reportP1Facades(): void {
    const active = EditorSelection.GetActiveObject();
    console.log(
        "[EditorScripts] selection: count=" + EditorSelection.GetObjectCount() +
        " active=" + (active ? active.GetName() : "<none>"));

    console.log(
        "[EditorScripts] history: canUndo=" + EditorHistory.CanUndo() +
        " undoCount=" + EditorHistory.GetUndoCount() +
        " redoCount=" + EditorHistory.GetRedoCount() +
        " recording=" + EditorHistory.IsRecording());

    // Probe an asset that ships with the engine, so this prints something
    // meaningful in any checkout instead of depending on the open project.
    // A .zasset on purpose: GetAssetType reads the ZASS header (or derives the
    // type for YAML authoring assets), so a plain .object.json legitimately
    // reports an empty type and would make this self-check look broken.
    const probe = "asset/objects/character/player/components/animation/data/anim.skeleton_map.zasset";
    console.log(
        "[EditorScripts] assets: exists=" + EditorAssetDatabase.Exists(probe) +
        " type='" + EditorAssetDatabase.GetAssetType(probe) +
        "' guid='" + EditorAssetDatabase.GetGuid(probe) + "'");
}

export function OnEditorLoad(): void {
    reportEnvironment();
    reportP1Facades();
}

export function OnEditorUnload(): void {
    console.log("[EditorScripts] Bootstrap unloading.");
}
