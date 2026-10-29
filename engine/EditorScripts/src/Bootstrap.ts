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

// ---------------------------------------------------------------------------
// P2: menu items and Inspector buttons.
//
// Handlers are exported BY NAME (EditorMenu/EditorInspector take a module id
// and a function name, not a closure) because ScriptEnv can only invoke
// `module.exports.<fn>()`. That is also why they have to be exported: a
// module-local function is invisible to the invoker.
// ---------------------------------------------------------------------------

export function OnToolsSayHello(): void {
    console.log("[EditorScripts] Tools > Say Hello clicked. project='" +
        EditorEnvironment.GetProjectRoot() + "'");
}

export function OnToolsReportSelection(): void {
    const count = EditorSelection.GetObjectCount();
    const active = EditorSelection.GetActiveObject();
    console.log("[EditorScripts] Tools > Report Selection: count=" + count +
        " active=" + (active ? active.GetName() : "<none>"));
}

export function OnInspectAnimationComponent(): void {
    console.log("[EditorScripts] Inspector button on AnimationComponent clicked.");
}

function registerP2Hooks(): void {
    // "Report/..." nests: Tools > Report > Selection.
    EditorMenu.AddItem("Say Hello", "Bootstrap", "OnToolsSayHello", 0);
    EditorMenu.AddItem("Report/Selection", "Bootstrap", "OnToolsReportSelection", 10);
    EditorInspector.AddButton("AnimationComponent", "Log From Script",
                              "Bootstrap", "OnInspectAnimationComponent");

    console.log("[EditorScripts] hooks: menuItems=" + EditorMenu.GetItemCount() +
        " first='" + EditorMenu.GetItemPathAt(0) +
        "' animButtons=" + EditorInspector.GetButtonCount("AnimationComponent"));

    // Invoke both through the registry, which is the only way to exercise a
    // menu row / Inspector button headlessly -- a ZSlate click cannot be
    // synthesised from script. Proves the whole path: registry lookup ->
    // InvokeExportedFunction -> back into this module.
    console.log("[EditorScripts] hooks: invokeMenu=" + EditorMenu.InvokeItem("Say Hello") +
        " invokeButton=" + EditorInspector.InvokeButton("AnimationComponent", "Log From Script") +
        " invokeMissing=" + EditorMenu.InvokeItem("No Such Item"));
}

// ---------------------------------------------------------------------------
// P3 self-check: prove a scripted edit is actually undoable.
//
// Unlike the P1/P2 checks this one MUTATES -- there is no other way to show the
// record/undo round-trip works. It is made safe by being a closed loop: move,
// undo, assert the old value is back. Net scene change is zero, and it drops
// the redo entry its own undo created so the user cannot Ctrl+Shift+Z into a
// mystery move.
//
// NOT run from OnEditorLoad: the editor VM initialises at PostInit, BEFORE a
// level is loaded, so there are no scene objects yet at that point. It is
// registered as a Tools menu item and driven after the world exists -- from the
// menu, or headlessly with
//   --cvar "exec.after <frames> script.menu Self Test/Undo Round Trip"
// ---------------------------------------------------------------------------

export function OnSelfTestUndoRoundTrip(): void {
    // Wall_6 in the shipped smoke level: static geometry nothing else keys off.
    // Missing (different project / no level yet) means skip, not fail.
    const probe = GameObject.Find("Wall_6");
    if (!probe) {
        console.log("[EditorScripts] undo check: skipped (no probe object in this level)");
        return;
    }

    const transform = probe.GetTransform();
    const before = transform.GetLocalPosition();

    EditorHistory.BeginTransaction("Script Undo Probe");
    // BEFORE the mutation: Record* snapshots current state, so recording after
    // the write would store the already-changed value and undo would no-op.
    EditorHistory.RecordGameObject(probe, "Move Probe");
    // Vector3 is a `declare interface` -- engine value types marshal as plain
    // JS objects, so it is an object literal, not `new Vector3(...)`.
    transform.SetLocalPosition({ x: before.x + 5, y: before.y, z: before.z });
    EditorHistory.EndTransaction();

    const moved = transform.GetLocalPosition();
    const undoTitle = EditorHistory.GetUndoTitle();
    const undone = EditorHistory.Undo();
    const after = transform.GetLocalPosition();
    const restored = Math.abs(after.x - before.x) < 0.0001;

    console.log("[EditorScripts] undo check: before.x=" + before.x +
        " moved.x=" + moved.x + " title='" + undoTitle + "'" +
        " undone=" + undone + " after.x=" + after.x + " restored=" + restored);

    if (!restored) {
        console.error("[EditorScripts] undo check FAILED: position was not restored");
    }

    // Discard the redo entry the Undo above produced, so this check leaves no
    // trace in the user's history.
    EditorHistory.ClearHistory("editor script self-test");
}

// ---------------------------------------------------------------------------
// P4 self-check: container marshalling (std::vector<T> <-> JS array).
//
// Covers both directions, because they fail independently: ToJs builds a JS
// array from a vector, FromJs walks a JS array back into one. Needs a loaded
// level for the selection half, so it runs from the console hook like the P3
// check rather than from OnEditorLoad.
// ---------------------------------------------------------------------------

export function OnSelfTestContainers(): void {
    // --- C++ -> JS: a real registry query returns a string[] ---------------
    const skelMaps = EditorAssetDatabase.FindAssets("", "AnimSkelMap", "");
    const counted = EditorAssetDatabase.CountAssets("", "AnimSkelMap", "");
    const isArray = Array.isArray(skelMaps);
    console.log("[EditorScripts] containers out: isArray=" + isArray +
        " length=" + skelMaps.length + " count=" + counted +
        " agree=" + (skelMaps.length === counted) +
        " first='" + (skelMaps.length > 0 ? skelMaps[0] : "") + "'");

    // --- JS -> C++: hand a string[] in and see it take effect --------------
    const before = EditorSelection.GetInstanceIds();
    const selected = EditorSelection.SelectByNames(["Wall_1", "Wall_2", "no_such_object"]);
    const ids = EditorSelection.GetInstanceIds();
    const active = EditorSelection.GetActiveObject();
    console.log("[EditorScripts] containers in: requested=3 selected=" + selected +
        " idsLength=" + ids.length + " idsIsArray=" + Array.isArray(ids) +
        " active=" + (active ? active.GetName() : "<none>"));

    // Restore whatever was selected before, so the check leaves no trace.
    if (before.length === 0) {
        EditorSelection.Clear();
    }
}

export function OnEditorLoad(): void {
    reportEnvironment();
    reportP1Facades();
    registerP2Hooks();
    EditorMenu.AddItem("Self Test/Undo Round Trip", "Bootstrap", "OnSelfTestUndoRoundTrip", 20);
    EditorMenu.AddItem("Self Test/Containers", "Bootstrap", "OnSelfTestContainers", 21);
}

export function OnEditorUnload(): void {
    console.log("[EditorScripts] Bootstrap unloading.");
}
