#include "InspectorDataTableInspector.h"

#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Asset/Data/DataTable.h"

#include <string>

// Detect "this asset is a DataTable wrapper" without committing to a specific
// wrapper class. Cheap path: peek the type name from the .zasset header,
// resolve to Type*, walk parents up to DataTableBase. Returns the Type* on
// match (so the caller doesn't have to repeat the lookup) or nullptr.
//
// The DataTable inspector UI now lives natively in ZSlateInspectorWindow
// (BuildDataTableAsset); this TU only exposes the type-detection helper that
// the dispatcher uses to route a .zasset to that builder.
const Type* ResolveDataTableType(const std::filesystem::path& asset_path)
{
    auto asset_manager = GET_SYSTEM(AssetManager);
    if (asset_manager == nullptr)
    {
        return nullptr;
    }
    const std::string class_name = asset_manager->GetAssetTypeName(asset_path);
    if (class_name.empty())
    {
        return nullptr;
    }
    const Type* asset_type = TypeManager::GetInstance().ClassNameToType(class_name.c_str());
    if (asset_type == nullptr)
    {
        return nullptr;
    }

    // Walk the parent chain. We deliberately do NOT use Type::IsBaseOf here
    // because that is the "this is a base of typeIndex X" test -- we'd need
    // to flip orientation. The parent walk is O(depth) and depth is <=4 for
    // any DataTable subclass (Object -> DataTableBase -> DataTable<TRow> ->
    // user wrapper), so the cost is negligible.
    const Type* base_type = TypeOf<DataTableBase>();
    for (const Type* cursor = asset_type; cursor != nullptr; cursor = cursor->base)
    {
        if (cursor == base_type)
        {
            return asset_type;
        }
    }
    return nullptr;
}
