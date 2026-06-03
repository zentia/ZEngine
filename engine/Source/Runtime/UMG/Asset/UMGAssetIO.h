#pragma once

#include <memory>
#include <string>

namespace ZUMG
{
class UWidget;
class UUserWidget;

// Persist a live UMG widget tree to a `.zasset` (binary SerializedFile) at the
// project-relative URL. Creates a transient UWidgetAsset, flattens the tree into
// it, and writes through AssetManager::saveAsset. Returns false on failure.
bool SaveWidgetTreeAsset(const std::shared_ptr<UWidget>& root, const std::string& asset_url);

// Load a `.zasset` widget tree and wrap it in a (prebuilt) UUserWidget ready for
// AddToViewport(). Returns nullptr if the asset can't be loaded / is empty.
std::shared_ptr<UUserWidget> LoadUserWidgetFromAsset(const std::string& asset_url);

// Load a `.zasset` widget tree and return the raw root UWidget (no UserWidget
// wrapper). Used by the editor designer which edits the tree in place.
std::shared_ptr<UWidget> LoadWidgetTreeAsset(const std::string& asset_url);
}  // namespace ZUMG
