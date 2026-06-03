// =============================================================================
// WeaponDataTable -- editor-side schema registration.
//
// This translation unit lives in ZEditor (NOT ZRuntime) because it pulls in
// the editor-only DataTableImporter / CsvSchemaRegistry. Runtime players
// don't need a CSV parser; they only read the pre-baked .zasset products.
//
// The companion runtime file is
//   engine/Source/Runtime/Resource/Asset/Data/Demo/WeaponDataTable.{h,cpp}
// which carries the actual class definition + IMPLEMENT_DATA_TABLE.
// =============================================================================

#include "Editor/AssetPipeline/DataTableImporter/DataTableImporter.h"
#include "Runtime/Resource/Asset/Data/Demo/WeaponDataTable.h"

#include <cstdlib>  // strtol / strtof
#include <string>

namespace
{
    // Look up a column by header name. Returns the cell value or an empty
    // string if the column doesn't exist in this CSV. Designers can drop
    // optional columns without breaking the Import (the row field just
    // keeps its default value).
    const eastl::string& cell(const eastl::vector<eastl::string>& cells,
                              const eastl::vector<eastl::string>& headers,
                              const char* name)
    {
        static const eastl::string empty;
        for (size_t i = 0; i < headers.size(); ++i)
        {
            if (headers[i] == name)
            {
                return i < cells.size() ? cells[i] : empty;
            }
        }
        return empty;
    }

    int32_t toInt(const eastl::string& s)
    {
        if (s.empty())
        {
            return 0;
        }
        return static_cast<int32_t>(std::strtol(s.c_str(), nullptr, 10));
    }

    float toFloat(const eastl::string& s)
    {
        if (s.empty())
        {
            return 0.f;
        }
        return std::strtof(s.c_str(), nullptr);
    }
}  // namespace

REGISTER_DATA_TABLE(
    WeaponDataTable,
    WeaponRow,
    "Weapon",
    // Applier: maps cells to row fields by header name. The "id" column is
    // mandatory and is filled by the user (we keep this convention so the
    // applier stays symmetric across all columns).
    [](WeaponRow& row,
       const eastl::vector<eastl::string>& cells,
       const eastl::vector<eastl::string>& headers) {
        row.id = cell(cells, headers, "id");
        row.display_name = cell(cells, headers, "display_name");
        row.damage = toInt(cell(cells, headers, "damage"));
        row.cooldown = toFloat(cell(cells, headers, "cooldown"));
        row.icon = cell(cells, headers, "icon");
    });
