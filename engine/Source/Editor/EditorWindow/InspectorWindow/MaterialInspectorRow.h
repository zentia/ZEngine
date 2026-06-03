#pragma once

#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Math/Vector3.h"

#include <EASTL/string.h>

#include <string>

class Texture2D; of one editable material property row, shared by the
// legacy ImGui drawer and the native ZSlate inspector. Each row carries the row
// kind plus raw pointers into the live Material fields/property structs, so a
// UI layer can render an editor widget and write back without duplicating the
// (fairly involved) shader-property -> material-field mapping rules.
enum class MaterialInspectorRowKind
{
    Color,
    Float,
    Bool,
    String,
    Texture
};

struct MaterialInspectorRow
{
    MaterialInspectorRowKind kind {MaterialInspectorRowKind::Float};
    std::string label;

    Vector3* color {nullptr};  // Color: rgb
    float* alpha {nullptr};    // Color: alpha (may be null => no alpha channel)

    float* value {nullptr};    // Float
    float range_min {0.0f};
    float range_max {1.0f};

    bool* boolean {nullptr};       // Bool
    eastl::string* str {nullptr};  // String (legacy / misc)

    PPtr<Texture2D>* texture {nullptr};  // Texture2D asset reference
};
