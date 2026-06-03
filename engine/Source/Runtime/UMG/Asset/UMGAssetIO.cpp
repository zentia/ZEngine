#include "Runtime/UMG/Asset/UMGAssetIO.h"

#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/UMG/Asset/UMGWidgetSerializer.h"
#include "Runtime/UMG/Asset/UWidgetAsset.h"
#include "Runtime/UMG/Core/UUserWidget.h"
#include "Runtime/UMG/Core/UWidget.h"

namespace ZUMG
{
bool SaveWidgetTreeAsset(const std::shared_ptr<UWidget>& root, const std::string& asset_url)
{
    if (!root)
        return false;

    auto object_manager = GET_SYSTEM(ObjectManager);
    auto asset_manager = GET_SYSTEM(AssetManager);
    if (!object_manager || !asset_manager)
        return false;

    Object* produced = object_manager->Produce(TypeOf<UWidgetAsset>(), 0);
    if (produced == nullptr)
        return false;

    auto* asset = static_cast<UWidgetAsset*>(produced);
    SerializeWidgetTree(root, *asset);

    const bool ok = asset_manager->saveAsset(*asset, eastl::string(asset_url.c_str()));

    MemoryManager::DestroyObject(produced);
    return ok;
}

std::shared_ptr<UUserWidget> LoadUserWidgetFromAsset(const std::string& asset_url)
{
    auto asset_manager = GET_SYSTEM(AssetManager);
    if (!asset_manager)
        return nullptr;

    UWidgetAsset* asset = asset_manager->loadAsset<UWidgetAsset>(eastl::string(asset_url.c_str()));
    if (asset == nullptr)
        return nullptr;

    auto user_widget = std::make_shared<UUserWidget>();
    if (!user_widget->LoadFromAsset(*asset))
        return nullptr;
    return user_widget;
}

std::shared_ptr<UWidget> LoadWidgetTreeAsset(const std::string& asset_url)
{
    auto asset_manager = GET_SYSTEM(AssetManager);
    if (!asset_manager)
        return nullptr;

    UWidgetAsset* asset = asset_manager->loadAsset<UWidgetAsset>(eastl::string(asset_url.c_str()));
    if (asset == nullptr)
        return nullptr;
    return BuildWidgetTree(*asset);
}
}  // namespace ZUMG
