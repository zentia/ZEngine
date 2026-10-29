#pragma once

#include "ZSlate/Core/SlateGeometry.h"

#include <memory>
#include <vector>

namespace ZSlate
{

class SWidget;

// =============================================================================
// FHitTestGrid — spatial hit-testing accelerator (UE FHittestGrid analogue)
// =============================================================================
//
// Divides the screen into a coarse grid of cells. Each cell tracks which
// widgets intersect it. On mouse move, hit-test only scans the cell(s)
// under the cursor rather than walking the full widget tree.
//
// This is a simple bucketed grid, not a recursive quadtree. For ZSlate's
// typical widget counts (< 500 per frame), cell-based lookup is sufficient.
//
class FHitTestGrid
{
public:
    FHitTestGrid(float cellSize = 64.0f) : m_CellSize(cellSize) {}

    // Clear all entries (call each frame before rebuilding)
    void Clear()
    {
        m_Cells.clear();
        m_Widgets.clear();
    }

    // Register a widget with its screen-space bounding rect
    void AddWidget(std::shared_ptr<SWidget> widget, const UIRect& screenRect)
    {
        if (!widget || !widget->IsHitTestVisible()) return;

        int32_t idx = static_cast<int32_t>(m_Widgets.size());
        m_Widgets.push_back({widget, screenRect});

        // Insert into overlapping cells
        int32_t minCx = GridX(screenRect.x);
        int32_t minCy = GridY(screenRect.y);
        int32_t maxCx = GridX(screenRect.Right());
        int32_t maxCy = GridY(screenRect.Bottom());

        for (int32_t cy = minCy; cy <= maxCy; ++cy)
        {
            for (int32_t cx = minCx; cx <= maxCx; ++cx)
            {
                auto key = Key(cx, cy);
                auto& cell = m_Cells[key];
                cell.push_back(idx);
            }
        }
    }

    // Find the topmost (last-registered) hit-testable widget under screenPos.
    // Returns nullptr if nothing is under the cursor.
    std::shared_ptr<SWidget> HitTest(const Vector2& screenPos) const
    {
        int32_t cx = GridX(screenPos.x);
        int32_t cy = GridY(screenPos.y);
        auto key = Key(cx, cy);

        auto it = m_Cells.find(key);
        if (it == m_Cells.end()) return nullptr;

        // Iterate in reverse (last-registered = topmost in Z-order)
        for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit)
        {
            const auto& entry = m_Widgets[*rit];
            if (entry.Rect.Contains(screenPos.x, screenPos.y))
                return entry.Widget;
        }
        return nullptr;
    }

    // Collect all widgets under screenPos (for multi-layer hit-testing)
    void CollectWidgets(const Vector2& screenPos, std::vector<std::shared_ptr<SWidget>>& out) const
    {
        int32_t cx = GridX(screenPos.x);
        int32_t cy = GridY(screenPos.y);
        auto key = Key(cx, cy);

        auto it = m_Cells.find(key);
        if (it == m_Cells.end()) return;

        for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit)
        {
            const auto& entry = m_Widgets[*rit];
            if (entry.Rect.Contains(screenPos.x, screenPos.y))
                out.push_back(entry.Widget);
        }
    }

private:
    struct Entry { std::shared_ptr<SWidget> Widget; UIRect Rect; };

    float m_CellSize;
    std::vector<Entry> m_Widgets;

    // Cell key: (col,row) packed into int64
    struct CellKey
    {
        int64_t value;
        CellKey(int32_t cx, int32_t cy) : value((static_cast<int64_t>(cx) << 32) | static_cast<uint32_t>(cy)) {}
        bool operator==(const CellKey& o) const { return value == o.value; }
    };
    struct CellKeyHash { size_t operator()(CellKey k) const { return std::hash<int64_t>{}(k.value); } };

    std::unordered_map<CellKey, std::vector<int32_t>, CellKeyHash> m_Cells;

    int32_t GridX(float x) const { return static_cast<int32_t>(std::floor(x / m_CellSize)); }
    int32_t GridY(float y) const { return static_cast<int32_t>(std::floor(y / m_CellSize)); }
    static CellKey Key(int32_t cx, int32_t cy) { return CellKey(cx, cy); }
};

}  // namespace ZSlate
