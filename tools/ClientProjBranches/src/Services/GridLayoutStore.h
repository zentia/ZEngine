#pragma once

// ============================================================================
// Services/GridLayoutStore.h
//
// Persists user-customised table layouts (column order + widths) to
// grid_layout.json, byte-compatible with the C# GridLayoutStore:
//
//   {
//     "BranchesTab": {
//       "Name": { "DisplayIndex": 0, "Width": 180 },
//       "Path": { "DisplayIndex": 1, "Width": 475 }
//     }
//   }
//
// One entry per logical table id ("BranchesTab", "GitBranchesTab", "TipsTab"…).
// A corrupt file is ignored — layout state must never keep the UI from opening.
// ============================================================================

#include "JsonConfig.h"
#include "Paths.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ClientProj {
namespace GridLayout {

struct ColumnState
{
    int DisplayIndex = -1;
    int Width        = -1;
};

using TableLayout = std::vector<std::pair<std::string, ColumnState>>;   // ordered by DisplayIndex

inline std::unordered_map<std::string, TableLayout> LoadAll()
{
    std::unordered_map<std::string, TableLayout> result;

    const Json::Object root = Json::LoadFromFile(Paths::GridLayout());
    for (const auto& table : root)
    {
        if (!table.second || !table.second->IsObject()) continue;

        TableLayout columns;
        for (const auto& column : table.second->AsObject())
        {
            if (!column.second || !column.second->IsObject()) continue;

            ColumnState state;
            state.DisplayIndex = Json::OptInt(column.second->AsObject(), "DisplayIndex", -1);
            state.Width        = Json::OptInt(column.second->AsObject(), "Width", -1);
            columns.emplace_back(column.first, state);
        }

        // Same ordering rule as DataGridView.DisplayIndex, with -1 last.
        std::stable_sort(columns.begin(), columns.end(),
            [](const auto& a, const auto& b) {
                const int ia = a.second.DisplayIndex < 0 ? 1000000 : a.second.DisplayIndex;
                const int ib = b.second.DisplayIndex < 0 ? 1000000 : b.second.DisplayIndex;
                if (ia != ib) return ia < ib;
                return a.first < b.first;
            });

        result[table.first] = std::move(columns);
    }

    return result;
}

inline TableLayout Get(const std::string& tableId)
{
    const auto all = LoadAll();
    const auto it  = all.find(tableId);
    return (it == all.end()) ? TableLayout{} : it->second;
}

// Column name -> width, in display order. Anything omitted keeps its default.
inline std::vector<std::pair<std::string, float>> OrderedWidths(const std::string& tableId)
{
    std::vector<std::pair<std::string, float>> widths;
    for (const auto& column : Get(tableId))
    {
        if (column.second.Width < 0) continue;
        widths.emplace_back(column.first, static_cast<float>(column.second.Width));
    }
    return widths;
}

// `columns` is (name, width) in display order. DisplayIndex is written so the
// C# side restores the exact same order.
inline void Save(const std::string& tableId,
                 const std::vector<std::pair<std::string, float>>& columns)
{
    if (tableId.empty()) return;

    auto all = LoadAll();

    Json::Object map;
    int displayIndex = 0;
    for (const auto& column : columns)
    {
        Json::Object state;
        state["DisplayIndex"] = std::make_shared<Json::Value>(static_cast<double>(displayIndex++));
        state["Width"]        = std::make_shared<Json::Value>(
            static_cast<double>(static_cast<int>(column.second + 0.5f)));
        map[column.first] = std::make_shared<Json::Value>(state);
    }

    // Merge: other tables keep whatever they had.
    Json::Object root;
    for (const auto& table : Json::LoadFromFile(Paths::GridLayout()))
    {
        if (table.first == tableId || !table.second) continue;
        root[table.first] = table.second;
    }
    root[tableId] = std::make_shared<Json::Value>(map);

    Json::SaveToFile(Paths::GridLayout(), root);
}

} } // namespace ClientProj::GridLayout
