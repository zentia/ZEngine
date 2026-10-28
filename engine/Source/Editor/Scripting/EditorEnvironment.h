#pragma once

#include "Runtime/BaseClasses/Object.h"

#include <eastl/string.h>

#include "EditorEnvironment.generated.h"

// Editor-VM only: script-facing facade over editor / project state.
//
// This is the first class bound into the Editor script domain (the
// UnityEditor.dll equivalent). ZHeaderTool stamps every binding of the ZEditor
// module as ZScriptDomain::Editor, so `EditorEnvironment` is published only
// into the editor-scripting VM and declared only in zengine-editor.d.ts --
// game scripts never see it.
//
// Same shape as `Time` (Runtime/Scripting/ScriptTime): ZCLASS needs an Object
// derivative, so a static facade is an Object subclass that is never
// instantiated. Static ZFUNCTIONs are exposed as `EditorEnvironment.Foo()`.
//
// Deliberately tiny: P0 only has to prove the whole chain (ZEditor
// reflection -> editor d.ts -> editor VM -> TS module). Selection / Undo /
// AssetDatabase facades land in P1 next to this one.
ZCLASS()
class EditorEnvironment : public Object
{
    ZGENERATED_BODY(EditorEnvironment)

public:
    /// Absolute, forward-slash root of the open project; "" when none is open.
    ZFUNCTION()
    static eastl::string GetProjectRoot();

    /// True while Play-In-Editor is running (playing or paused).
    ZFUNCTION()
    static bool IsPlaying();

    /// True while Play-In-Editor is paused.
    ZFUNCTION()
    static bool IsPaused();
};
