#pragma once

#include <memory>
#include <string>

class UWidgetAsset;

namespace ZUMG
{
class UWidget;

// Factory: create an empty UWidget of the given class (UWidget::GetWidgetClassName
// identity). Returns nullptr for unknown class names.
std::shared_ptr<UWidget> CreateWidgetByClassName(const std::string& class_name);

// Flatten a live UWidget tree into a UWidgetAsset node array (clears out_asset
// first). Captures per-widget properties and, for box/overlay parents, the
// per-child slot layout.
void SerializeWidgetTree(const std::shared_ptr<UWidget>& root, UWidgetAsset& out_asset);

// Rebuild a live UWidget tree from a UWidgetAsset. Returns the root widget (the
// node whose parent index is -1), or nullptr if the asset is empty / malformed.
std::shared_ptr<UWidget> BuildWidgetTree(const UWidgetAsset& asset);
}  // namespace ZUMG
