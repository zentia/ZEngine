#pragma once

// =============================================================================
// WeaponDataTable -- the canonical "hello world" DataTable demo.
//
// Demonstrates the full per-game cost of adding a new data table:
//
//   1. Define a plain struct WeaponRow with eastl::string id + your fields.
//      Add DECLARE_SERIALIZE + a template Transfer() listing every field.
//
//   2. DECLARE_DATA_TABLE(WeaponDataTable, WeaponRow) in the header.
//
//   3. In the .cpp:
//      - IMPLEMENT_DATA_TABLE(WeaponDataTable, WeaponRow);
//      - REGISTER_DATA_TABLE(WeaponDataTable, WeaponRow, "Weapon",
//                            [](WeaponRow& row, ...){ /* CSV -> fields */ });
//
// That's the entire user contract. Any <Project>/Data/Weapon.csv (or
// /Data/<anywhere>/Weapon.csv) is automatically compiled into a
// WeaponDataTable .zasset on editor startup. Runtime code looks up rows
// with `weapon_table->find("sword_iron")` after loadAsset<WeaponDataTable>.
//
// We intentionally place this in Runtime/ (not Editor/) and live-link it
// into ZRuntime so the demo's wrapper class is visible to AssetManager
// at *load* time too -- otherwise prefabs that PPtr a WeaponDataTable
// would fail to deserialise in the runtime player.
// =============================================================================

#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Resource/Asset/Data/DataTable.h"

#include <EASTL/string.h>
#include <cstdint>

// -----------------------------------------------------------------------------
// Row struct. Plain struct, NOT an Object. First serialised field MUST be
// `id` (primary key contract; see DataTable.h header doc).
// -----------------------------------------------------------------------------
struct WeaponRow
{
    DECLARE_SERIALIZE(WeaponRow)

    eastl::string id;            ///< primary key; CSV column "id"
    eastl::string display_name;  ///< CSV column "display_name"
    int32_t damage = 0;          ///< CSV column "damage"
    float cooldown = 0.f;        ///< CSV column "cooldown" (seconds)
    eastl::string icon;          ///< CSV column "icon" (asset path)
};

// -----------------------------------------------------------------------------
// Transfer() lives inline in the header because WeaponRow is a plain struct;
// no IMPLEMENT_SERIALIZE counterpart is needed (Object-derived classes need
// IMPLEMENT_OBJECT_SERAILIZE; structs don't). Field order here is what
// determines the on-disk layout, NOT the order in the struct declaration --
// keep them in sync with the CSV column order to avoid surprises when a
// reader walks both side by side.
// -----------------------------------------------------------------------------
template<typename TransferFunction>
inline void WeaponRow::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(id, "id");
    transfer.Transfer(display_name, "display_name");
    transfer.Transfer(damage, "damage");
    transfer.Transfer(cooldown, "cooldown");
    transfer.Transfer(icon, "icon");
}

// -----------------------------------------------------------------------------
// The wrapper class. One line, courtesy of DECLARE_DATA_TABLE.
// -----------------------------------------------------------------------------
DECLARE_DATA_TABLE(WeaponDataTable, WeaponRow);
