#pragma once

// =============================================================================
// LocalizationTable -- ZEngine's DataTable for localised UI strings.
//
// Why this exists
// ---------------
// Several ZEngine executables ship to machines that have no project and no
// editor (ZEnvironmentCollector is the motivating case: it is a WINDOWS
// subsystem tool zipped to a player, and it needs Simplified Chinese labels
// without hard-coding 150 string literals into a translation unit).
//
// The strings therefore live in a real DataTable, which means:
//   * they are data -- a translator edits `ui_strings.csv`, not a .cpp;
//   * they are loaded through the same AssetManager / SerializedFile path every
//     other asset uses, so no second bespoke parser exists;
//   * they are cooked to `.zasset` at build time (DataTableCook), so the tool
//     that ships never parses CSV at run time.
//
// Shape
// -----
// One table per language, one row per string id:
//
//   id,text
//   tab.collection,环境采集
//   ...
//
// `id` is the primary key (DataTable.h row contract), `text` is the translated
// string. English deliberately has NO table: it stays the literal written at the
// call site, exactly like ZSlate's localization model
// (ZSlate/Core/SlateLocalization.h) -- a missing row degrades to English rather
// than to an empty label.
//
// Where the pieces live
// ---------------------
//   * This header + LocalizationTable.cpp are in Runtime so the wrapper class is
//     visible to AssetManager at LOAD time (same reasoning as WeaponDataTable:
//     a runtime consumer must be able to deserialise the .zasset).
//   * The CSV parse + bake lives in the tool-facing C API
//     (Runtime/ZEngineApi/ZEngineToolApi.cpp) instead of in the Editor's
//     CsvSchemaRegistry: ZRuntime must not depend on ZEditor, and the cook step
//     has to be runnable from a build tree that never opens a project.
// =============================================================================

#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Resource/Asset/Data/DataTable.h"

#include <EASTL/string.h>

// -----------------------------------------------------------------------------
// Row struct. Plain struct, NOT an Object. `id` must stay the FIRST transferred
// field -- it is the row's primary key (see DataTable.h).
// -----------------------------------------------------------------------------
struct LocalizationRow
{
    DECLARE_SERIALIZE(LocalizationRow)

    eastl::string id;    ///< primary key; CSV column "id"
    eastl::string text;  ///< CSV column "text"
};

template<typename TransferFunction>
inline void LocalizationRow::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(id, "id");
    transfer.Transfer(text, "text");
}

DECLARE_DATA_TABLE(LocalizationDataTable, LocalizationRow);
