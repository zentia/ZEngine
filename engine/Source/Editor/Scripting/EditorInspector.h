#pragma once

#include "Runtime/BaseClasses/Object.h"

#include <eastl/string.h>

#include "EditorInspector.generated.h"

// Editor-VM only: extend the Inspector from TypeScript (P2).
//
// Scope: BUTTONS attached to a component type. A script registers
//
//     EditorInspector.AddButton("AnimationComponent", "Validate Clips",
//                               "Bootstrap", "OnValidateClips");
//
// and the Inspector renders a "Validate Clips" button inside every
// AnimationComponent section; clicking it calls that exported function.
//
// Why buttons and not a full custom inspector
// -------------------------------------------
// Unity's CustomEditor lets a script DRAW the whole section. That needs a
// widget-construction API across the script boundary -- ZFUNCTION marshals
// scalars, strings and bound-class pointers, so there is no way to hand a
// script an SVerticalBox and let it add rows. Pretending otherwise would mean
// inventing a retained-mode shadow tree, which is a much larger piece of work
// than P2.
//
// Buttons are the useful 80%: the common custom inspector in practice is the
// stock field list plus a few tool actions ("Bake", "Validate", "Reset"), and
// the stock list is already generated from the TypeTree. Scripts that need to
// read or write the fields themselves do it through EditorSelection +
// the component's own bound API, not by drawing.
//
// `component_type` is matched EXACTLY against Component::GetTypeName() -- no
// base-class walk, so a button on a base type does not leak onto every derived
// component.
//
// Registering the same (type, label) twice replaces the handler, so a script
// reload does not stack duplicate buttons.
ZCLASS()
class EditorInspector : public Object
{
    ZGENERATED_BODY(EditorInspector)

public:
    /// Adds a button to every Inspector section of `component_type`.
    /// Returns false when any argument is empty.
    ZFUNCTION()
    static bool AddButton(const eastl::string& component_type,
                          const eastl::string& label,
                          const eastl::string& module,
                          const eastl::string& function_name);

    /// Removes every button registered for `component_type`; returns how many.
    ZFUNCTION()
    static int ClearButtons(const eastl::string& component_type);

    /// How many buttons are registered for `component_type`.
    ZFUNCTION()
    static int GetButtonCount(const eastl::string& component_type);

    /// Total registered across all component types.
    ZFUNCTION()
    static int GetTotalButtonCount();

    /// Invokes a registered button's handler as if it were clicked. Returns
    /// false when that (type, label) is not registered or the handler threw.
    /// Headless testability, same rationale as EditorMenu.InvokeItem.
    ZFUNCTION()
    static bool InvokeButton(const eastl::string& component_type, const eastl::string& label);
};
