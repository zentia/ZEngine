#pragma once
#include "BaseClasses/Type.h"
#include "Runtime/BaseClasses/Object.h"

class AssetBundle : public Object
{
public:
    Object* GetImpl(const Type* type, std::filesystem::path& path);

    template<typename AssetType>
    AssetType* Get(const std::string& asset_path)
    {
        std::filesystem::path path(asset_path);
        return static_cast<AssetType*>(GetImpl(nullptr, path));
    }
};
