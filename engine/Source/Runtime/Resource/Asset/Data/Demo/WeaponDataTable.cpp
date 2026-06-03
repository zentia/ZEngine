#include "WeaponDataTable.h"

// =============================================================================
// Runtime-side instantiation only. The CSV-import schema registration
// (REGISTER_DATA_TABLE) is in the Editor sibling (WeaponDataTableEditor.cpp)
// because it pulls in the CsvSchemaRegistry from the editor-only
// data_table_importer.h, and ZRuntime must not depend on ZEditor.
//
// This split is by design:
//   - The wrapper CLASS (WeaponDataTable / DataTable<WeaponRow>) must be
//     present in BOTH Editor and Runtime so prefabs/scenes that PPtr a
//     WeaponDataTable can deserialise in the player too.
//   - The CSV->row APPLIER is editor-only -- the runtime player never
//     parses CSV; it only reads the pre-baked .zasset. Keeping the
//     applier in Editor saves a few KB in player builds and keeps the
//     dependency direction clean.
// =============================================================================

IMPLEMENT_DATA_TABLE(WeaponDataTable, WeaponRow);
