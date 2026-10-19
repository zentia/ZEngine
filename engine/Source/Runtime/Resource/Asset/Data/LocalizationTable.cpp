#include "LocalizationTable.h"

// Runtime-side instantiation only. See the header for why the CSV applier is
// NOT registered here (unlike WeaponDataTable, whose applier sits in
// Editor/AssetPipeline/DataTableImporter/Demo/): ZRuntime must not depend on
// ZEditor, and the cook step is driven from
// Runtime/ZEngineApi/ZEngineToolApi.cpp so a build tree with no project can
// still produce the .zasset.
IMPLEMENT_DATA_TABLE(LocalizationDataTable, LocalizationRow);
