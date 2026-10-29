#pragma once

#include "ZSlate/Core/SlateEnums.h"

#include <cstdint>

namespace ZSlate
{

// =============================================================================
// ITableRow — a row in a table/list/tree view (UE ITableRow analogue)
// =============================================================================

class ITableRow
{
public:
    virtual ~ITableRow() = default;

    // Called when the row is assigned to a new data item
    virtual void InitializeRow() {}

    // Called when the row's data item goes out of view
    virtual void ResetRow() {}

    // Selection state
    virtual bool IsSelected() const = 0;
    virtual void SetSelected(bool selected) = 0;

    // Expansion (for tree views)
    virtual bool IsItemExpanded() const { return false; }
    virtual void SetExpansionState(bool expanded) {}
    virtual bool CanExpand() const { return false; }

    // Indent level (for tree views)
    virtual int32_t GetIndentLevel() const { return 0; }
    virtual void SetIndentLevel(int32_t level) {}
};

}  // namespace ZSlate
